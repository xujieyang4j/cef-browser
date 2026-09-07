#include "profile/browsing_data_store.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextDocumentFragment>
#include <QUrl>

#include "settings/browser_settings.h"

namespace {

constexpr int kDataVersion = 1;
constexpr int kMaxBookmarks = 1000;
constexpr int kMaxHistoryEntries = 1000;
constexpr int kMaxDataBytes = 2 * 1024 * 1024;
constexpr int kMaxBookmarkHtmlBytes = 5 * 1024 * 1024;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
}

QDateTime ParseDate(const QJsonValue& value) {
  return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

QString DecodeHtml(const QString& value) {
  return QTextDocumentFragment::fromHtml(value).toPlainText();
}

bool IsSafeImportedUrl(const QString& value) {
  const QUrl url(value);
  const QString scheme = url.scheme().toLower();
  return url.isValid() && !url.host().isEmpty() &&
         (scheme == QStringLiteral("http") ||
          scheme == QStringLiteral("https"));
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
    const auto normalized = BrowserSettings::NormalizeStoredUrl(bookmark.url);
    if (normalized && *normalized != QStringLiteral("about:blank")) {
      bookmark.url = *normalized;
      loaded_bookmarks.append(bookmark);
    }
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
    const auto normalized = BrowserSettings::NormalizeStoredUrl(entry.url);
    if (normalized && *normalized != QStringLiteral("about:blank")) {
      entry.url = *normalized;
      loaded_history.append(entry);
    }
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

bool BrowsingDataStore::ImportBookmarksHtml(const QString& path,
                                            int* imported_count,
                                            QString* error) {
  if (imported_count) *imported_count = 0;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  if (file.size() > kMaxBookmarkHtmlBytes) {
    SetError(error, QStringLiteral("Bookmark file is unexpectedly large"));
    return false;
  }
  const QString html = QString::fromUtf8(file.readAll());
  const QRegularExpression anchor_pattern(
      QStringLiteral(R"(<a\b([^>]*)>(.*?)</a\s*>)"),
      QRegularExpression::CaseInsensitiveOption |
          QRegularExpression::DotMatchesEverythingOption);
  const QRegularExpression href_pattern(
      QStringLiteral(
          R"REGEX(\bhref\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))REGEX"),
      QRegularExpression::CaseInsensitiveOption);

  QSet<QString> seen;
  for (const Bookmark& bookmark : bookmarks_) seen.insert(bookmark.url);
  QList<Bookmark> imported;
  QRegularExpressionMatchIterator matches = anchor_pattern.globalMatch(html);
  while (matches.hasNext() &&
         bookmarks_.size() + imported.size() < kMaxBookmarks) {
    const QRegularExpressionMatch anchor = matches.next();
    const QRegularExpressionMatch href =
        href_pattern.match(anchor.captured(1));
    if (!href.hasMatch()) continue;
    QString encoded_url = href.captured(1);
    if (encoded_url.isEmpty()) encoded_url = href.captured(2);
    if (encoded_url.isEmpty()) encoded_url = href.captured(3);
    const QString url = DecodeHtml(encoded_url).trimmed();
    if (!IsSafeImportedUrl(url) || seen.contains(url)) continue;
    seen.insert(url);
    imported.append(Bookmark{url, DecodeHtml(anchor.captured(2)).trimmed(),
                             QDateTime::currentDateTimeUtc()});
  }
  for (auto bookmark = imported.crbegin(); bookmark != imported.crend();
       ++bookmark) {
    bookmarks_.prepend(*bookmark);
  }
  if (imported_count) *imported_count = imported.size();
  return true;
}

bool BrowsingDataStore::ExportBookmarksHtml(const QString& path,
                                            QString* error) const {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    SetError(error, QStringLiteral("Unable to create export directory"));
    return false;
  }
  QByteArray html("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
                  "<META HTTP-EQUIV=\"Content-Type\" "
                  "CONTENT=\"text/html; charset=UTF-8\">\n"
                  "<TITLE>Trail Browser Bookmarks</TITLE>\n"
                  "<H1>Trail Browser Bookmarks</H1>\n<DL><p>\n");
  for (const Bookmark& bookmark : bookmarks_) {
    const QString title = bookmark.title.isEmpty() ? bookmark.url
                                                    : bookmark.title;
    html += QStringLiteral(
                "    <DT><A HREF=\"%1\" ADD_DATE=\"%2\">%3</A>\n")
                .arg(bookmark.url.toHtmlEscaped())
                .arg(bookmark.created_at.toSecsSinceEpoch())
                .arg(title.toHtmlEscaped())
                .toUtf8();
  }
  html += "</DL><p>\n";

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  if (file.write(html) != html.size()) {
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

bool BrowsingDataStore::RenameBookmark(const QString& url,
                                       const QString& title) {
  const auto found = std::find_if(
      bookmarks_.begin(), bookmarks_.end(),
      [&url](const Bookmark& bookmark) { return bookmark.url == url; });
  if (found == bookmarks_.end()) return false;
  found->title = title.trimmed();
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
  const auto normalized = BrowserSettings::NormalizeStoredUrl(url);
  return normalized && *normalized != QStringLiteral("about:blank");
}
