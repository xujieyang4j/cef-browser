#include "download/download_manager.h"

#include <algorithm>

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QUrl>

#include "include/wrapper/cef_helpers.h"

namespace {

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

DownloadManager::DownloadManager(QObject* parent) : QObject(parent) {
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

void DownloadManager::CancelDownload(quint32 id) {
  const auto callback = callbacks_.value(id);
  if (callback) callback->Cancel();
}

void DownloadManager::ClearFinished() {
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
  }

  emit DownloadChanged(item.id, is_new);
  const int current_active_count = active_count();
  if (current_active_count != previous_active_count) {
    emit ActiveCountChanged(current_active_count);
  }
}

bool DownloadManager::IsActive(State state) {
  return state == State::Starting || state == State::InProgress;
}
