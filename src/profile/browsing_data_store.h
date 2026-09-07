#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

class BrowsingDataStore final {
 public:
  struct Bookmark {
    QString url;
    QString title;
    QDateTime created_at;
  };

  struct HistoryEntry {
    QString url;
    QString title;
    QDateTime last_visited_at;
    int visit_count = 1;
  };

  explicit BrowsingDataStore(QString path);

  bool Load(QString* error = nullptr);
  bool Save(QString* error = nullptr) const;
  bool ImportBookmarksHtml(const QString& path, int* imported_count = nullptr,
                           QString* error = nullptr);
  bool ExportBookmarksHtml(const QString& path,
                           QString* error = nullptr) const;
  const QList<Bookmark>& bookmarks() const { return bookmarks_; }
  const QList<HistoryEntry>& history() const { return history_; }
  bool IsBookmarked(const QString& url) const;
  bool AddBookmark(const QString& url, const QString& title);
  bool RenameBookmark(const QString& url, const QString& title);
  bool RemoveBookmark(const QString& url);
  void RecordVisit(const QString& url, const QString& title,
                   QDateTime visited_at = QDateTime::currentDateTimeUtc());
  bool RemoveHistory(const QString& url);
  void ClearHistory();

 private:
  static bool IsRecordableUrl(const QString& url);

  QString path_;
  QList<Bookmark> bookmarks_;
  QList<HistoryEntry> history_;
};
