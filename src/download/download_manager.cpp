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

namespace {

constexpr int kHistoryVersion = 1;
constexpr int kMaxHistoryItems = 200;
constexpr int kMaxHistoryBytes = 2 * 1024 * 1024;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
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

DownloadManager::Item Snapshot(CefRefPtr<CefDownloadItem> download) {
  DownloadManager::Item item;
  item.id = download->GetId();
  item.full_path =
      QString::fromStdString(download->GetFullPath().ToString());
  item.file_name =
      QString::fromStdString(download->GetSuggestedFileName().ToString());
  if (!item.full_path.isEmpty()) {
    item.file_name = QFileInfo(item.full_path).fileName();
  }
  item.url = QString::fromStdString(download->GetURL().ToString());
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

  bool CanDownload(CefRefPtr<CefBrowser>, const CefString&,
                   const CefString&) override {
    CEF_REQUIRE_UI_THREAD();
    return manager_ != nullptr;
  }

  bool OnBeforeDownload(
      CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem> download_item,
      const CefString&, CefRefPtr<CefBeforeDownloadCallback> callback) override {
    CEF_REQUIRE_UI_THREAD();
    if (!manager_ || !download_item || !download_item->IsValid()) return false;

    manager_->UpdateDownload(Snapshot(download_item), nullptr);
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
    manager_->UpdateDownload(Snapshot(download_item), std::move(callback));
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
  if (file.size() > kMaxHistoryBytes) {
    SetError(error, QStringLiteral("Download history file is unexpectedly large"));
    return false;
  }

  QJsonParseError parse_error;
  const QJsonDocument document =
      QJsonDocument::fromJson(file.readAll(), &parse_error);
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
  for (int index = 0;
       index < std::min(static_cast<int>(downloads.size()), kMaxHistoryItems);
       ++index) {
    const QJsonObject object = downloads.at(index).toObject();
    const auto state =
        ParseState(object.value(QStringLiteral("state")).toString());
    if (!state) continue;
    Item item;
    item.id = restored_id--;
    item.file_name = object.value(QStringLiteral("fileName")).toString();
    item.full_path = object.value(QStringLiteral("fullPath")).toString();
    item.url = object.value(QStringLiteral("url")).toString();
    item.detail = object.value(QStringLiteral("detail")).toString();
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
    if (item.file_name.isEmpty() && item.full_path.isEmpty() &&
        item.url.isEmpty()) {
      continue;
    }
    loaded_items.insert(item.id, item);
    loaded_order.append(item.id);
  }

  items_ = std::move(loaded_items);
  order_ = std::move(loaded_order);
  callbacks_.clear();
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
    downloads.append(QJsonObject{
        {QStringLiteral("fileName"), found->file_name},
        {QStringLiteral("fullPath"), found->full_path},
        {QStringLiteral("url"), found->url},
        {QStringLiteral("detail"), found->detail},
        {QStringLiteral("receivedBytes"), found->received_bytes},
        {QStringLiteral("totalBytes"), found->total_bytes},
        {QStringLiteral("percent"), found->percent},
        {QStringLiteral("state"), state},
    });
    if (downloads.size() >= kMaxHistoryItems) break;
  }

  const QJsonObject root{
      {QStringLiteral("version"), kHistoryVersion},
      {QStringLiteral("downloads"), downloads},
  };
  QSaveFile file(history_path_);
  if (!file.open(QIODevice::WriteOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0) {
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

bool DownloadManager::ClearFinished() {
  const QList<quint32> ids = order_;
  for (const quint32 id : ids) {
    const auto found = items_.constFind(id);
    if (found != items_.cend() && !IsActive(found->state)) {
      items_.remove(id);
      order_.removeAll(id);
      callbacks_.remove(id);
      emit DownloadRemoved(id);
    }
  }
  QString error;
  const bool saved = SaveHistory(&error);
  if (!saved) emit PersistenceError(error);
  return saved;
}

bool DownloadManager::OpenDownload(quint32 id) const {
  const auto found = item(id);
  return found && !found->full_path.isEmpty() &&
         QDesktopServices::openUrl(QUrl::fromLocalFile(found->full_path));
}

bool DownloadManager::ShowDownloadInFolder(quint32 id) const {
  const auto found = item(id);
  if (!found || found->full_path.isEmpty()) return false;

  const QFileInfo file(found->full_path);
#if defined(OS_WIN)
  return QProcess::startDetached(
      QStringLiteral("explorer.exe"),
      {QStringLiteral("/select,%1")
           .arg(QDir::toNativeSeparators(file.absoluteFilePath()))});
#elif defined(OS_MAC)
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

void DownloadManager::UpdateForTesting(const Item& item) {
  UpdateDownload(item, nullptr);
}

void DownloadManager::UpdateDownload(
    const Item& item, CefRefPtr<CefDownloadItemCallback> callback) {
  const int previous_active_count = active_count();
  const bool is_new = !items_.contains(item.id);
  if (is_new) order_.prepend(item.id);
  items_.insert(item.id, item);

  if (IsActive(item.state) && callback) {
    callbacks_.insert(item.id, std::move(callback));
  } else if (!IsActive(item.state)) {
    callbacks_.remove(item.id);
    TrimFinishedHistory();
  }

  emit DownloadChanged(item.id, is_new);
  const int current_active_count = active_count();
  if (current_active_count != previous_active_count) {
    emit ActiveCountChanged(current_active_count);
  }
  if (!IsActive(item.state)) {
    QString error;
    if (!SaveHistory(&error)) emit PersistenceError(error);
  }
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
    emit DownloadRemoved(id);
    --finished_count;
  }
}

bool DownloadManager::IsActive(State state) {
  return state == State::Starting || state == State::InProgress ||
         state == State::Paused;
}
