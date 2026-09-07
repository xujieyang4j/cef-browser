#include "download/download_manager.h"

#include <algorithm>
#include <limits>

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QUrl>

#include "include/wrapper/cef_helpers.h"
#include "util/bounded_file.h"

namespace {

constexpr int kHistoryVersion = 1;
constexpr int kMaxHistoryItems = 200;
constexpr int kMaxActiveDownloads = 32;
constexpr int kMaxHistoryBytes = 2 * 1024 * 1024;
constexpr int kMaxSourceUrlBytes = 64 * 1024;
constexpr int kMaxLocalPathBytes = 32 * 1024;
constexpr int kMaxFileNameCharacters = 512;
constexpr int kMaxDetailCharacters = 1024;
constexpr int kMaxRequestMethodCharacters = 16;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
}

std::optional<QString> NormalizeSourceUrl(QString value) {
  value = value.trimmed();
  if (value.isEmpty() || value.size() > kMaxSourceUrlBytes ||
      value.toUtf8().size() > kMaxSourceUrlBytes ||
      value.contains(QChar::Null) || value.contains(QLatin1Char('\r')) ||
      value.contains(QLatin1Char('\n'))) {
    return std::nullopt;
  }
  QUrl url(value, QUrl::StrictMode);
  const QString scheme = url.scheme().toLower();
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
      (scheme != QStringLiteral("http") &&
       scheme != QStringLiteral("https"))) {
    return std::nullopt;
  }
  url.setScheme(scheme);
  const QString normalized = url.toString(QUrl::FullyEncoded);
  if (normalized.toUtf8().size() > kMaxSourceUrlBytes) return std::nullopt;
  return normalized;
}

std::optional<QString> NormalizeRuntimeSourceUrl(QString value) {
  value = value.trimmed();
  if (value.isEmpty() || value.size() > kMaxSourceUrlBytes ||
      value.toUtf8().size() > kMaxSourceUrlBytes ||
      value.contains(QChar::Null) || value.contains(QLatin1Char('\r')) ||
      value.contains(QLatin1Char('\n'))) {
    return std::nullopt;
  }
  QUrl url(value, QUrl::StrictMode);
  if (!url.isValid() || url.isRelative() || url.scheme().isEmpty()) {
    return std::nullopt;
  }
  if (!url.userInfo().isEmpty()) url.setUserInfo(QString());
  const QString normalized = url.toString(QUrl::FullyEncoded);
  if (normalized.toUtf8().size() > kMaxSourceUrlBytes) return std::nullopt;
  return normalized;
}

std::optional<QString> NormalizeLocalPath(const QString& value) {
  if (value.isEmpty() || value.contains(QChar::Null)) return std::nullopt;
  const QString path = QDir::cleanPath(QDir::fromNativeSeparators(value));
  if (!QDir::isAbsolutePath(path)) return std::nullopt;
  const QString normalized = QFileInfo(path).absoluteFilePath();
  if (normalized.toUtf8().size() > kMaxLocalPathBytes) return std::nullopt;
  return normalized;
}

QString NormalizeDisplayFileName(QString value) {
  if (value.contains(QChar::Null)) return QString();
  value = QFileInfo(QDir::fromNativeSeparators(value.trimmed())).fileName();
  for (qsizetype index = 0; index < value.size(); ++index) {
    if (value.at(index).category() == QChar::Other_Control) {
      value[index] = QLatin1Char(' ');
    }
  }
  value = value.simplified().left(kMaxFileNameCharacters);
  if (value == QStringLiteral(".") || value == QStringLiteral("..")) {
    return QString();
  }
  return value;
}

QString NormalizeDetail(QString value) {
  for (qsizetype index = 0; index < value.size(); ++index) {
    if (value.at(index).category() == QChar::Other_Control) {
      value[index] = QLatin1Char(' ');
    }
  }
  return value.simplified().left(kMaxDetailCharacters);
}

QString FileNameForRecord(const QString& path, const QString& stored_name,
                          const QString& url) {
  if (!path.isEmpty()) {
    return NormalizeDisplayFileName(QFileInfo(path).fileName());
  }
  const QString sanitized = NormalizeDisplayFileName(stored_name);
  if (!sanitized.isEmpty()) return sanitized;
  return NormalizeDisplayFileName(QUrl(url).path());
}

QString StateId(DownloadManager::State state) {
  switch (state) {
    case DownloadManager::State::Complete:
      return QStringLiteral("complete");
    case DownloadManager::State::Cancelled:
      return QStringLiteral("cancelled");
    case DownloadManager::State::Interrupted:
      return QStringLiteral("interrupted");
    case DownloadManager::State::Starting:
    case DownloadManager::State::InProgress:
    case DownloadManager::State::Paused:
      return QString();
  }
  return QString();
}

std::optional<DownloadManager::State> ParseState(const QString& value) {
  if (value == QStringLiteral("complete")) {
    return DownloadManager::State::Complete;
  }
  if (value == QStringLiteral("cancelled")) {
    return DownloadManager::State::Cancelled;
  }
  if (value == QStringLiteral("interrupted")) {
    return DownloadManager::State::Interrupted;
  }
  return std::nullopt;
}

QString BoundedCefString(const CefString& value, int max_characters) {
  size_t length =
      std::min(value.length(), static_cast<size_t>(max_characters));
  if (length == 0) return {};
  if (length < value.length() &&
      QChar::isHighSurrogate(
          static_cast<char32_t>(value.c_str()[length - 1]))) {
    --length;
  }
  return QString::fromUtf16(value.c_str(), static_cast<qsizetype>(length));
}

std::optional<QString> CheckedCefString(const CefString& value,
                                        int max_bytes) {
  if (value.length() > static_cast<size_t>(max_bytes)) return std::nullopt;
  QString converted = BoundedCefString(value, max_bytes);
  if (converted.toUtf8().size() > max_bytes) return std::nullopt;
  return converted;
}

std::optional<DownloadManager::Item> Snapshot(
    CefRefPtr<CefDownloadItem> download,
    const CefString* suggested_name = nullptr) {
  DownloadManager::Item item;
  item.id = download->GetId();
  const auto full_path =
      CheckedCefString(download->GetFullPath(), kMaxLocalPathBytes);
  const auto source_url =
      CheckedCefString(download->GetURL(), kMaxSourceUrlBytes);
  if (!source_url) return std::nullopt;
  item.full_path = full_path.value_or(QString());
  item.file_name = BoundedCefString(
      suggested_name && !suggested_name->empty()
          ? *suggested_name
          : download->GetSuggestedFileName(),
      kMaxFileNameCharacters);
  if (!item.full_path.isEmpty()) {
    item.file_name = QFileInfo(item.full_path).fileName();
  }
  item.url = *source_url;
  item.received_bytes = download->GetReceivedBytes();
  item.total_bytes = download->GetTotalBytes();
  item.bytes_per_second = download->GetCurrentSpeed();
  item.percent = download->GetPercentComplete();

  if (download->IsComplete()) {
    item.state = DownloadManager::State::Complete;
  } else if (download->IsCanceled()) {
    item.state = DownloadManager::State::Cancelled;
  } else if (download->IsInterrupted()) {
    item.state = DownloadManager::State::Interrupted;
    item.detail = QStringLiteral("Error %1")
                      .arg(static_cast<int>(download->GetInterruptReason()));
  } else if (download->IsPaused()) {
    item.state = DownloadManager::State::Paused;
  } else if (download->IsInProgress()) {
    item.state = DownloadManager::State::InProgress;
  } else {
    item.state = DownloadManager::State::Starting;
  }
  return item;
}

}  // namespace

class DownloadHandlerImpl final : public CefDownloadHandler {
 public:
  explicit DownloadHandlerImpl(DownloadManager* manager) : manager_(manager) {}

  bool CanDownload(CefRefPtr<CefBrowser>, const CefString& url,
                   const CefString& request_method) override {
    CEF_REQUIRE_UI_THREAD();
    if (!manager_) return false;
    const auto safe_url = CheckedCefString(url, kMaxSourceUrlBytes);
    const bool accepted =
        manager_->CanAcceptDownload() && safe_url &&
        NormalizeRuntimeSourceUrl(*safe_url).has_value() &&
        request_method.length() <= kMaxRequestMethodCharacters;
    if (!accepted) {
      manager_->ReportRejectedDownload(
          QStringLiteral("Download blocked by safety limits"));
    }
    return accepted;
  }

  bool OnBeforeDownload(
      CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> download_item,
      const CefString& suggested_name,
      CefRefPtr<CefBeforeDownloadCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();
    if (!manager_ || !download_item || !download_item->IsValid()) return false;

    const auto snapshot = Snapshot(download_item, &suggested_name);
    if (!snapshot) {
      manager_->ReportRejectedDownload(
          QStringLiteral("Download blocked by safety limits"));
      return false;
    }
    if (!manager_->UpdateDownload(*snapshot, nullptr)) return false;
    // Let CEF display the platform save dialog. Passing an empty path keeps the
    // server-provided filename while still allowing the user to choose exactly
    // where the file will be written.
    callback->Continue(CefString(), true);
    return true;
  }

  void OnDownloadUpdated(
      CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> download_item,
      CefRefPtr<CefDownloadItemCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();
    if (!manager_ || !download_item || !download_item->IsValid()) return;
    const auto snapshot = Snapshot(download_item);
    if (!snapshot) {
      if (callback) callback->Cancel();
      manager_->ReportRejectedDownload(
          QStringLiteral("Download metadata exceeded safe limits"));
      return;
    }
    manager_->UpdateDownload(*snapshot, std::move(callback));
  }

 private:
  QPointer<DownloadManager> manager_;

  IMPLEMENT_REFCOUNTING(DownloadHandlerImpl);
  DISALLOW_COPY_AND_ASSIGN(DownloadHandlerImpl);
};

DownloadManager::DownloadManager(QString history_path, QObject* parent)
    : QObject(parent), history_path_(std::move(history_path)) {
  handler_ = new DownloadHandlerImpl(this);
}

DownloadManager::~DownloadManager() {
  callbacks_.clear();
  handler_ = nullptr;
}

QList<DownloadManager::Item> DownloadManager::items() const {
  QList<Item> result;
  result.reserve(order_.size());
  for (const quint32 id : order_) {
    if (items_.contains(id)) result.append(items_.value(id));
  }
  return result;
}

std::optional<DownloadManager::Item> DownloadManager::item(quint32 id) const {
  const auto found = items_.constFind(id);
  if (found == items_.cend()) return std::nullopt;
  return found.value();
}

int DownloadManager::active_count() const {
  return std::count_if(items_.cbegin(), items_.cend(),
                       [](const Item& item) { return IsActive(item.state); });
}

bool DownloadManager::LoadHistory(QString* error) {
  if (history_path_.isEmpty()) return true;
  QFile file(history_path_);
  if (!file.exists()) return true;
  if (!file.open(QIODevice::ReadOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  const auto bytes = trail::ReadBoundedFile(
      file, kMaxHistoryBytes,
      QStringLiteral("Download history file is unexpectedly large"), error);
  if (!bytes) return false;

  QJsonParseError parse_error;
  const QJsonDocument document =
      QJsonDocument::fromJson(*bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    SetError(error, parse_error.errorString());
    return false;
  }
  const QJsonObject root = document.object();
  if (root.value(QStringLiteral("version")).toInt() != kHistoryVersion) {
    SetError(error, QStringLiteral("Unsupported download history version"));
    return false;
  }

  QHash<quint32, Item> loaded_items;
  QList<quint32> loaded_order;
  quint32 restored_id = std::numeric_limits<quint32>::max();
  const QJsonArray downloads =
      root.value(QStringLiteral("downloads")).toArray();
  for (int index = 0; index < downloads.size() &&
                      loaded_order.size() < kMaxHistoryItems;
       ++index) {
    const QJsonObject object = downloads.at(index).toObject();
    const auto state =
        ParseState(object.value(QStringLiteral("state")).toString());
    if (!state) continue;
    const auto url =
        NormalizeSourceUrl(object.value(QStringLiteral("url")).toString());
    if (!url) continue;
    const auto full_path = NormalizeLocalPath(
        object.value(QStringLiteral("fullPath")).toString());
    Item item;
    item.id = restored_id--;
    item.full_path = full_path.value_or(QString());
    item.url = *url;
    item.file_name = FileNameForRecord(
        item.full_path, object.value(QStringLiteral("fileName")).toString(),
        item.url);
    item.detail =
        NormalizeDetail(object.value(QStringLiteral("detail")).toString());
    item.received_bytes =
        std::max<qint64>(0, object.value(QStringLiteral("receivedBytes"))
                                .toVariant()
                                .toLongLong());
    item.total_bytes =
        std::max<qint64>(0, object.value(QStringLiteral("totalBytes"))
                                .toVariant()
                                .toLongLong());
    item.percent =
        std::clamp(object.value(QStringLiteral("percent")).toInt(-1), -1,
                   100);
    item.state = *state;
    loaded_items.insert(item.id, item);
    loaded_order.append(item.id);
  }

  items_ = std::move(loaded_items);
  order_ = std::move(loaded_order);
  callbacks_.clear();
  runtime_local_path_ids_.clear();
  return true;
}

bool DownloadManager::SaveHistory(QString* error) const {
  if (history_path_.isEmpty()) return true;
  if (!QDir().mkpath(QFileInfo(history_path_).absolutePath())) {
    SetError(error, QStringLiteral("Unable to create profile directory"));
    return false;
  }

  QJsonArray downloads;
  for (const quint32 id : order_) {
    const auto found = items_.constFind(id);
    if (found == items_.cend()) continue;
    const QString state = StateId(found->state);
    if (state.isEmpty()) continue;
    const auto url = NormalizeSourceUrl(found->url);
    if (!url) continue;
    const QString full_path =
        NormalizeLocalPath(found->full_path).value_or(QString());
    downloads.append(QJsonObject{
        {QStringLiteral("fileName"),
         FileNameForRecord(full_path, found->file_name, *url)},
        {QStringLiteral("fullPath"), full_path},
        {QStringLiteral("url"), *url},
        {QStringLiteral("detail"), NormalizeDetail(found->detail)},
        {QStringLiteral("receivedBytes"), found->received_bytes},
        {QStringLiteral("totalBytes"), found->total_bytes},
        {QStringLiteral("percent"), found->percent},
        {QStringLiteral("state"), state},
    });
    if (downloads.size() >= kMaxHistoryItems) break;
  }

  auto serialize = [&downloads] {
    return QJsonDocument(QJsonObject{
                             {QStringLiteral("version"), kHistoryVersion},
                             {QStringLiteral("downloads"), downloads},
                         })
        .toJson(QJsonDocument::Compact);
  };
  QByteArray bytes = serialize();
  while (bytes.size() > kMaxHistoryBytes && !downloads.isEmpty()) {
    downloads.removeLast();
    bytes = serialize();
  }
  QSaveFile file(history_path_);
  if (!file.open(QIODevice::WriteOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  if (file.write(bytes) != bytes.size()) {
    SetError(error, file.errorString());
    file.cancelWriting();
    return false;
  }
  if (!file.commit()) {
    SetError(error, file.errorString());
    return false;
  }
  return true;
}

void DownloadManager::CancelDownload(quint32 id) {
  const auto callback = callbacks_.value(id);
  if (callback) callback->Cancel();
}

void DownloadManager::CancelAllActive() {
  QList<CefRefPtr<CefDownloadItemCallback>> active_callbacks;
  for (auto found = items_.cbegin(); found != items_.cend(); ++found) {
    const auto callback = callbacks_.value(found.key());
    if (IsActive(found->state) && callback) {
      active_callbacks.append(callback);
    }
  }
  for (const auto& callback : active_callbacks) {
    callback->Cancel();
  }
}

void DownloadManager::PauseDownload(quint32 id) {
  const auto callback = callbacks_.value(id);
  if (callback) callback->Pause();
}

void DownloadManager::ResumeDownload(quint32 id) {
  const auto callback = callbacks_.value(id);
  if (callback) callback->Resume();
}

bool DownloadManager::RemoveDownload(quint32 id) {
  const auto found = items_.constFind(id);
  if (found == items_.cend() || IsActive(found->state)) return false;

  const Item removed = found.value();
  const int order_index = order_.indexOf(id);
  items_.remove(id);
  order_.removeAll(id);
  callbacks_.remove(id);
  const bool runtime_path = runtime_local_path_ids_.remove(id);
  QString error;
  if (!SaveHistory(&error)) {
    items_.insert(id, removed);
    order_.insert(std::max(0, order_index), id);
    if (runtime_path) runtime_local_path_ids_.insert(id);
    emit PersistenceError(error);
    return false;
  }
  emit DownloadRemoved(id);
  return true;
}

bool DownloadManager::ClearFinished() {
  const QHash<quint32, Item> previous_items = items_;
  const QList<quint32> previous_order = order_;
  const QSet<quint32> previous_runtime_paths = runtime_local_path_ids_;
  QList<quint32> removed_ids;
  const QList<quint32> ids = order_;
  for (const quint32 id : ids) {
    const auto found = items_.constFind(id);
    if (found != items_.cend() && !IsActive(found->state)) {
      items_.remove(id);
      order_.removeAll(id);
      callbacks_.remove(id);
      runtime_local_path_ids_.remove(id);
      removed_ids.append(id);
    }
  }
  QString error;
  const bool saved = SaveHistory(&error);
  if (!saved) {
    items_ = previous_items;
    order_ = previous_order;
    runtime_local_path_ids_ = previous_runtime_paths;
    emit PersistenceError(error);
    return false;
  }
  for (const quint32 id : removed_ids) emit DownloadRemoved(id);
  return saved;
}

bool DownloadManager::CanOpenDownload(quint32 id) const {
  const auto found = item(id);
  if (!found || found->state != State::Complete ||
      !runtime_local_path_ids_.contains(id)) {
    return false;
  }
  const auto path = NormalizeLocalPath(found->full_path);
  if (!path) return false;
  const QFileInfo file(*path);
  return file.exists() && file.isFile() && !file.isSymLink();
}

bool DownloadManager::CanShowDownloadInFolder(quint32 id) const {
  const auto found = item(id);
  if (!found || IsActive(found->state) ||
      !runtime_local_path_ids_.contains(id)) {
    return false;
  }
  const auto path = NormalizeLocalPath(found->full_path);
  return path && QDir(QFileInfo(*path).absolutePath()).exists();
}

bool DownloadManager::OpenDownload(quint32 id) const {
  if (!CanOpenDownload(id)) return false;
  const QFileInfo file(*NormalizeLocalPath(item(id)->full_path));
  return QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath()));
}

bool DownloadManager::ShowDownloadInFolder(quint32 id) const {
  if (!CanShowDownloadInFolder(id)) return false;

  const QFileInfo file(*NormalizeLocalPath(item(id)->full_path));
#if defined(OS_WIN)
  if (!file.exists()) {
    return QProcess::startDetached(QStringLiteral("explorer.exe"),
                                   {QDir::toNativeSeparators(
                                       file.absolutePath())});
  }
  return QProcess::startDetached(
      QStringLiteral("explorer.exe"),
      {QStringLiteral("/select,%1")
           .arg(QDir::toNativeSeparators(file.absoluteFilePath()))});
#elif defined(OS_MAC)
  if (!file.exists()) {
    return QProcess::startDetached(QStringLiteral("open"),
                                   {file.absolutePath()});
  }
  return QProcess::startDetached(QStringLiteral("open"),
                                 {QStringLiteral("-R"), file.absoluteFilePath()});
#else
  return QDesktopServices::openUrl(QUrl::fromLocalFile(file.absolutePath()));
#endif
}

QString DownloadManager::FormatBytes(qint64 bytes) {
  if (bytes < 0) return QStringLiteral("—");
  constexpr qint64 kUnit = 1024;
  if (bytes < kUnit) return QStringLiteral("%1 B").arg(bytes);
  if (bytes < kUnit * kUnit) {
    return QStringLiteral("%1 KB").arg(bytes / static_cast<double>(kUnit), 0, 'f', 1);
  }
  if (bytes < kUnit * kUnit * kUnit) {
    return QStringLiteral("%1 MB")
        .arg(bytes / static_cast<double>(kUnit * kUnit), 0, 'f', 1);
  }
  return QStringLiteral("%1 GB")
      .arg(bytes / static_cast<double>(kUnit * kUnit * kUnit), 0, 'f', 1);
}

QString DownloadManager::FormatSpeed(qint64 bytes_per_second) {
  return bytes_per_second > 0
             ? QStringLiteral("%1/s").arg(FormatBytes(bytes_per_second))
             : QString();
}

QString DownloadManager::StatusText(const Item& item) {
  switch (item.state) {
    case State::Starting:
      return QStringLiteral("Waiting for save location…");
    case State::InProgress: {
      QString progress = item.percent >= 0
                             ? QStringLiteral("%1%").arg(item.percent)
                             : FormatBytes(item.received_bytes);
      const QString speed = FormatSpeed(item.bytes_per_second);
      if (!speed.isEmpty()) progress += QStringLiteral(" · %1").arg(speed);
      return progress;
    }
    case State::Paused:
      return item.percent >= 0 ? QStringLiteral("Paused · %1%").arg(item.percent)
                               : QStringLiteral("Paused");
    case State::Complete:
      return QStringLiteral("Complete");
    case State::Cancelled:
      return QStringLiteral("Cancelled");
    case State::Interrupted:
      return item.detail.isEmpty() ? QStringLiteral("Interrupted")
                                   : QStringLiteral("Interrupted · %1")
                                         .arg(item.detail);
  }
  return QString();
}

bool DownloadManager::UpdateForTesting(const Item& item) {
  return UpdateDownload(item, nullptr);
}

int DownloadManager::MaxActiveDownloadsForTesting() {
  return kMaxActiveDownloads;
}

bool DownloadManager::CanAcceptDownload(quint32 id) const {
  return (id != 0 && items_.contains(id)) ||
         active_count() < kMaxActiveDownloads;
}

bool DownloadManager::UpdateDownload(
    const Item& item, CefRefPtr<CefDownloadItemCallback> callback) {
  const int previous_active_count = active_count();
  const bool is_new = !items_.contains(item.id);
  if (is_new && IsActive(item.state) && !CanAcceptDownload(item.id)) {
    if (callback) callback->Cancel();
    ReportRejectedDownload(
        QStringLiteral("Too many downloads are already active"));
    return false;
  }
  const auto source_url = NormalizeRuntimeSourceUrl(item.url);
  if (!source_url) {
    if (callback) callback->Cancel();
    ReportRejectedDownload(
        QStringLiteral("Download source metadata was invalid"));
    return false;
  }
  if (is_new) order_.prepend(item.id);
  Item normalized = item;
  normalized.url = *source_url;
  const auto local_path = NormalizeLocalPath(item.full_path);
  normalized.full_path = local_path.value_or(QString());
  if (local_path) {
    normalized.file_name =
        NormalizeDisplayFileName(QFileInfo(*local_path).fileName());
    runtime_local_path_ids_.insert(item.id);
  } else {
    normalized.file_name = NormalizeDisplayFileName(item.file_name);
    runtime_local_path_ids_.remove(item.id);
  }
  normalized.detail = NormalizeDetail(item.detail);
  normalized.received_bytes = std::max<qint64>(0, item.received_bytes);
  normalized.total_bytes = std::max<qint64>(0, item.total_bytes);
  normalized.bytes_per_second = std::max<qint64>(0, item.bytes_per_second);
  normalized.percent = std::clamp(item.percent, -1, 100);
  items_.insert(item.id, normalized);

  if (IsActive(normalized.state) && callback) {
    callbacks_.insert(item.id, std::move(callback));
  } else if (!IsActive(normalized.state)) {
    callbacks_.remove(item.id);
    TrimFinishedHistory();
  }

  emit DownloadChanged(item.id, is_new);
  const int current_active_count = active_count();
  if (current_active_count != previous_active_count) {
    emit ActiveCountChanged(current_active_count);
  }
  if (!IsActive(normalized.state)) {
    QString error;
    if (!SaveHistory(&error)) emit PersistenceError(error);
  }
  return true;
}

void DownloadManager::ReportRejectedDownload(const QString& reason) {
  emit DownloadRejected(reason);
}

void DownloadManager::TrimFinishedHistory() {
  int finished_count =
      std::count_if(items_.cbegin(), items_.cend(),
                    [](const Item& item) { return !IsActive(item.state); });
  for (int index = order_.size() - 1;
       index >= 0 && finished_count > kMaxHistoryItems; --index) {
    const quint32 id = order_.at(index);
    const auto found = items_.constFind(id);
    if (found == items_.cend() || IsActive(found->state)) continue;
    items_.remove(id);
    order_.removeAt(index);
    callbacks_.remove(id);
    runtime_local_path_ids_.remove(id);
    emit DownloadRemoved(id);
    --finished_count;
  }
}

bool DownloadManager::IsActive(State state) {
  return state == State::Starting || state == State::InProgress ||
         state == State::Paused;
}
