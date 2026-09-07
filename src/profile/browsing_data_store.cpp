#include "profile/browsing_data_store.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

namespace {

constexpr int kDataVersion = 1;
constexpr int kMaxBookmarks = 1000;
constexpr int kMaxHistoryEntries = 1000;
constexpr int kMaxDataBytes = 2 * 1024 * 1024;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
}

QDateTime ParseDate(const QJsonValue& value) {
  return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

}  // namespace

BrowsingDataStore::BrowsingDataStore(QString path) : path_(std::move(path)) {}

bool BrowsingDataStore::Load(QString* error) {
  QFile file(path_);
  if (!file.exists()) return true;
  if (!file.open(QIODevice::ReadOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  if (file.size() > kMaxDataBytes) {
    SetError(error, QStringLiteral("Browsing data file is unexpectedly large"));
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
  if (root.value(QStringLiteral("version")).toInt() != kDataVersion) {
    SetError(error, QStringLiteral("Unsupported browsing data version"));
    return false;
  }

  QList<Bookmark> loaded_bookmarks;
  const QJsonArray bookmarks = root.value(QStringLiteral("bookmarks")).toArray();
  for (int index = 0;
       index < std::min(static_cast<int>(bookmarks.size()), kMaxBookmarks);
       ++index) {
    const QJsonObject object = bookmarks.at(index).toObject();
    Bookmark bookmark{object.value(QStringLiteral("url")).toString(),
                      object.value(QStringLiteral("title")).toString(),
                      ParseDate(object.value(QStringLiteral("createdAt")))};
    if (IsRecordableUrl(bookmark.url)) loaded_bookmarks.append(bookmark);
  }

  QList<HistoryEntry> loaded_history;
  const QJsonArray history = root.value(QStringLiteral("history")).toArray();
  for (int index = 0;
       index < std::min(static_cast<int>(history.size()), kMaxHistoryEntries);
       ++index) {
    const QJsonObject object = history.at(index).toObject();
    HistoryEntry entry{
        object.value(QStringLiteral("url")).toString(),
        object.value(QStringLiteral("title")).toString(),
        ParseDate(object.value(QStringLiteral("lastVisitedAt"))),
        std::max(1, object.value(QStringLiteral("visitCount")).toInt(1))};
    if (IsRecordableUrl(entry.url)) loaded_history.append(entry);
  }

  bookmarks_ = std::move(loaded_bookmarks);
  history_ = std::move(loaded_history);
  return true;
}

bool BrowsingDataStore::Save(QString* error) const {
  if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
    SetError(error, QStringLiteral("Unable to create profile directory"));
    return false;
  }
  QJsonArray bookmarks;
  for (const Bookmark& bookmark : bookmarks_) {
    bookmarks.append(QJsonObject{
        {QStringLiteral("url"), bookmark.url},
        {QStringLiteral("title"), bookmark.title},
        {QStringLiteral("createdAt"),
         bookmark.created_at.toString(Qt::ISODateWithMs)},
    });
  }
  QJsonArray history;
  for (const HistoryEntry& entry : history_) {
    history.append(QJsonObject{
        {QStringLiteral("url"), entry.url},
        {QStringLiteral("title"), entry.title},
        {QStringLiteral("lastVisitedAt"),
         entry.last_visited_at.toString(Qt::ISODateWithMs)},
        {QStringLiteral("visitCount"), entry.visit_count},
    });
  }

  const QJsonObject root{
      {QStringLiteral("version"), kDataVersion},
      {QStringLiteral("bookmarks"), bookmarks},
      {QStringLiteral("history"), history},
  };
  QSaveFile file(path_);
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

bool BrowsingDataStore::IsBookmarked(const QString& url) const {
  return std::any_of(bookmarks_.cbegin(), bookmarks_.cend(),
                     [&url](const Bookmark& bookmark) {
                       return bookmark.url == url;
                     });
}

bool BrowsingDataStore::AddBookmark(const QString& url, const QString& title) {
  if (!IsRecordableUrl(url) || IsBookmarked(url) ||
      bookmarks_.size() >= kMaxBookmarks) {
    return false;
  }
  bookmarks_.prepend(
      Bookmark{url, title.trimmed(), QDateTime::currentDateTimeUtc()});
  return true;
}

bool BrowsingDataStore::RemoveBookmark(const QString& url) {
  const auto found = std::find_if(
      bookmarks_.begin(), bookmarks_.end(),
      [&url](const Bookmark& bookmark) { return bookmark.url == url; });
  if (found == bookmarks_.end()) return false;
  bookmarks_.erase(found);
  return true;
}

void BrowsingDataStore::RecordVisit(const QString& url, const QString& title,
                                    QDateTime visited_at) {
  if (!IsRecordableUrl(url)) return;
  const auto found = std::find_if(
      history_.begin(), history_.end(),
      [&url](const HistoryEntry& entry) { return entry.url == url; });
  if (found != history_.end()) {
    HistoryEntry entry = *found;
    history_.erase(found);
    entry.title = title.trimmed();
    entry.last_visited_at = visited_at;
    ++entry.visit_count;
    history_.prepend(std::move(entry));
  } else {
    history_.prepend(HistoryEntry{url, title.trimmed(), visited_at, 1});
  }
  while (history_.size() > kMaxHistoryEntries) history_.removeLast();
}

bool BrowsingDataStore::RemoveHistory(const QString& url) {
  const auto found = std::find_if(
      history_.begin(), history_.end(),
      [&url](const HistoryEntry& entry) { return entry.url == url; });
  if (found == history_.end()) return false;
  history_.erase(found);
  return true;
}

void BrowsingDataStore::ClearHistory() {
  history_.clear();
}

bool BrowsingDataStore::IsRecordableUrl(const QString& url) {
  const QUrl parsed(url);
  return parsed.isValid() && !url.isEmpty() && url != QStringLiteral("about:blank") &&
         parsed.scheme() != QStringLiteral("data");
}
