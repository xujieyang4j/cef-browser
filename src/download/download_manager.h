#pragma once

#include <optional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
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

  explicit DownloadManager(QString history_path = {},
                           QObject* parent = nullptr);
  ~DownloadManager() override;

  CefRefPtr<CefDownloadHandler> handler() const { return handler_; }
  QList<Item> items() const;
  std::optional<Item> item(quint32 id) const;
  int active_count() const;
  bool LoadHistory(QString* error = nullptr);
  bool SaveHistory(QString* error = nullptr) const;

  void CancelDownload(quint32 id);
  void CancelAllActive();
  void PauseDownload(quint32 id);
  void ResumeDownload(quint32 id);
  bool RemoveDownload(quint32 id);
  bool ClearFinished();
  bool CanOpenDownload(quint32 id) const;
  bool CanShowDownloadInFolder(quint32 id) const;
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
  void PersistenceError(const QString& error);

 private:
  friend class DownloadHandlerImpl;

  void UpdateDownload(const Item& item,
                      CefRefPtr<CefDownloadItemCallback> callback);
  void TrimFinishedHistory();
  static bool IsActive(State state);

  CefRefPtr<CefDownloadHandler> handler_;
  QHash<quint32, Item> items_;
  QList<quint32> order_;
  QHash<quint32, CefRefPtr<CefDownloadItemCallback>> callbacks_;
  // History is editable profile data. Only paths delivered by CEF during the
  // current process may invoke operating-system file actions.
  QSet<quint32> runtime_local_path_ids_;
  QString history_path_;

  Q_DISABLE_COPY_MOVE(DownloadManager)
};
