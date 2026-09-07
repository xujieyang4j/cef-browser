#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include "include/cef_download_handler.h"

class DownloadHandlerImpl;

class DownloadManager final : public QObject {
  Q_OBJECT

 public:
  enum class State {
    Starting,
    InProgress,
    Paused,
    Complete,
    Cancelled,
    Interrupted,
  };

  struct Item {
    quint32 id = 0;
    QString file_name;
    QString full_path;
    QString url;
    QString detail;
    qint64 received_bytes = 0;
    qint64 total_bytes = 0;
    qint64 bytes_per_second = 0;
    int percent = -1;
    State state = State::Starting;
  };

  explicit DownloadManager(QObject* parent = nullptr);
  ~DownloadManager() override;

  CefRefPtr<CefDownloadHandler> handler() const { return handler_; }
  QList<Item> items() const;
  std::optional<Item> item(quint32 id) const;
  int active_count() const;

  void CancelDownload(quint32 id);
  void CancelAllActive();
  void PauseDownload(quint32 id);
  void ResumeDownload(quint32 id);
  void ClearFinished();
  bool OpenDownload(quint32 id) const;
  bool ShowDownloadInFolder(quint32 id) const;

  static QString FormatBytes(qint64 bytes);
  static QString FormatSpeed(qint64 bytes_per_second);
  static QString StatusText(const Item& item);

  // Keeps the download model and UI independently smoke-testable without
  // requiring an external HTTP server or writing a file to disk.
  void UpdateForTesting(const Item& item);

 signals:
  void DownloadChanged(quint32 id, bool is_new);
  void DownloadRemoved(quint32 id);
  void ActiveCountChanged(int active_count);

 private:
  friend class DownloadHandlerImpl;

  void UpdateDownload(const Item& item,
                      CefRefPtr<CefDownloadItemCallback> callback);
  static bool IsActive(State state);

  CefRefPtr<CefDownloadHandler> handler_;
  QHash<quint32, Item> items_;
  QList<quint32> order_;
  QHash<quint32, CefRefPtr<CefDownloadItemCallback>> callbacks_;

  Q_DISABLE_COPY_MOVE(DownloadManager)
};
