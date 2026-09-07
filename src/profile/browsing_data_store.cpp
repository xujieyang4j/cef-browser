#include "profile/browsing_data_store.h"

#include <algorithm>
#include <limits>
#include <optional>

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
#include "util/bounded_file.h"

namespace {

constexpr int kDataVersion = 1;
constexpr int kMaxBookmarks = 1000;
constexpr int kMaxHistoryEntries = 1000;
constexpr int kMaxDataBytes = 2 * 1024 * 1024;
constexpr int kMaxBookmarkHtmlBytes = 5 * 1024 * 1024;
constexpr int kMaxTitleCharacters = 512;
constexpr int kMaxRecordUrlBytes = 64 * 1024;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
}

QString NormalizeTitle(QString value) {
  for (qsizetype index = 0; index < value.size(); ++index) {
    if (value.at(index).category() == QChar::Other_Control) {
      value[index] = QLatin1Char(' ');
    }
  }
  return value.simplified().left(kMaxTitleCharacters);
}

QDateTime NormalizeDate(QDateTime value) {
  return value.isValid() ? value.toUTC()
                         : QDateTime::fromSecsSinceEpoch(0, Qt::UTC);
}

QDateTime ParseDate(const QJsonValue& value) {
  return NormalizeDate(
      QDateTime::fromString(value.toString(), Qt::ISODateWithMs));
}

std::optional<QString> NormalizeRecordableUrl(QString value) {
  const auto normalized =
      BrowserSettings::NormalizeStoredUrl(std::move(value));
  if (!normalized || *normalized == QStringLiteral("about:blank") ||
      normalized->toUtf8().size() > kMaxRecordUrlBytes) {
    return std::nullopt;
  }
  return normalized;
}

QString DecodeHtml(const QString& value) {
  return QTextDocumentFragment::fromHtml(value).toPlainText();
}

std::optional<QString> NormalizeImportedUrl(QString value) {
  const auto normalized = NormalizeRecordableUrl(std::move(value));
  if (!normalized) return std::nullopt;
  const QString scheme = QUrl(*normalized).scheme();
  if (scheme != QStringLiteral("http") &&
      scheme != QStringLiteral("https")) {
    return std::nullopt;
  }
  return normalized;
}

QJsonArray Prefix(const QJsonArray& values, int count) {
  QJsonArray result;
  for (int index = 0; index < count; ++index) result.append(values.at(index));
  return result;
}

QByteArray SerializeData(const QJsonArray& bookmarks,
                         const QJsonArray& history) {
  return QJsonDocument(QJsonObject{
                           {QStringLiteral("version"), kDataVersion},
                           {QStringLiteral("bookmarks"), bookmarks},
                           {QStringLiteral("history"), history},
                       })
      .toJson(QJsonDocument::Compact);
}

QJsonArray SerializeBookmarks(
    const QList<BrowsingDataStore::Bookmark>& bookmarks) {
  QJsonArray result;
  for (const BrowsingDataStore::Bookmark& bookmark : bookmarks) {
    const auto url = NormalizeRecordableUrl(bookmark.url);
    if (!url) continue;
    result.append(QJsonObject{
        {QStringLiteral("url"), *url},
        {QStringLiteral("title"), NormalizeTitle(bookmark.title)},
        {QStringLiteral("createdAt"),
         NormalizeDate(bookmark.created_at).toString(Qt::ISODateWithMs)},
    });
    if (result.size() >= kMaxBookmarks) break;
  }
  return result;
}

bool BookmarksFitDataBudget(
    const QList<BrowsingDataStore::Bookmark>& bookmarks) {
  return SerializeData(SerializeBookmarks(bookmarks), {}).size() <=
         kMaxDataBytes;
}

int LargestHistoryPrefix(const QJsonArray& bookmarks,
                         const QJsonArray& history) {
  int low = 0;
  int high = history.size();
  while (low < high) {
    const int middle = low + (high - low + 1) / 2;
    if (SerializeData(bookmarks, Prefix(history, middle)).size() <=
        kMaxDataBytes) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  return low;
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
  const auto bytes = trail::ReadBoundedFile(
      file, kMaxDataBytes,
      QStringLiteral("Browsing data file is unexpectedly large"), error);
  if (!bytes) return false;

  QJsonParseError parse_error;
  const QJsonDocument document =
      QJsonDocument::fromJson(*bytes, &parse_error);
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
  QSet<QString> seen_bookmarks;
  const QJsonArray bookmarks = root.value(QStringLiteral("bookmarks")).toArray();
  for (int index = 0; index < bookmarks.size() &&
                      loaded_bookmarks.size() < kMaxBookmarks;
       ++index) {
    const QJsonObject object = bookmarks.at(index).toObject();
    Bookmark bookmark{object.value(QStringLiteral("url")).toString(),
                      NormalizeTitle(
                          object.value(QStringLiteral("title")).toString()),
                      ParseDate(object.value(QStringLiteral("createdAt")))};
    const auto normalized = NormalizeRecordableUrl(bookmark.url);
    if (normalized && !seen_bookmarks.contains(*normalized)) {
      bookmark.url = *normalized;
      seen_bookmarks.insert(bookmark.url);
      loaded_bookmarks.append(bookmark);
    }
  }

  QList<HistoryEntry> loaded_history;
  QSet<QString> seen_history;
  const QJsonArray history = root.value(QStringLiteral("history")).toArray();
  for (int index = 0; index < history.size() &&
                      loaded_history.size() < kMaxHistoryEntries;
       ++index) {
    const QJsonObject object = history.at(index).toObject();
    HistoryEntry entry{
        object.value(QStringLiteral("url")).toString(),
        NormalizeTitle(object.value(QStringLiteral("title")).toString()),
        ParseDate(object.value(QStringLiteral("lastVisitedAt"))),
        std::clamp(object.value(QStringLiteral("visitCount")).toInt(1), 1,
                   std::numeric_limits<int>::max())};
    const auto normalized = NormalizeRecordableUrl(entry.url);
    if (normalized && !seen_history.contains(*normalized)) {
      entry.url = *normalized;
      seen_history.insert(entry.url);
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
  QJsonArray bookmarks = SerializeBookmarks(bookmarks_);
  if (SerializeData(bookmarks, {}).size() > kMaxDataBytes) {
    SetError(error,
             QStringLiteral("Bookmarks exceed the profile size limit"));
    return false;
  }
  QJsonArray history;
  for (const HistoryEntry& entry : history_) {
    const auto url = NormalizeRecordableUrl(entry.url);
    if (!url) continue;
    history.append(QJsonObject{
        {QStringLiteral("url"), *url},
        {QStringLiteral("title"), NormalizeTitle(entry.title)},
        {QStringLiteral("lastVisitedAt"),
         NormalizeDate(entry.last_visited_at).toString(Qt::ISODateWithMs)},
        {QStringLiteral("visitCount"), std::max(1, entry.visit_count)},
    });
    if (history.size() >= kMaxHistoryEntries) break;
  }

  QByteArray bytes = SerializeData(bookmarks, history);
  if (bytes.size() > kMaxDataBytes) {
    history = Prefix(history, LargestHistoryPrefix(bookmarks, history));
    bytes = SerializeData(bookmarks, history);
  }
  QSaveFile file(path_);
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

bool BrowsingDataStore::ImportBookmarksHtml(const QString& path,
                                            int* imported_count,
                                            QString* error) {
  if (imported_count) *imported_count = 0;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  const auto bytes = trail::ReadBoundedFile(
      file, kMaxBookmarkHtmlBytes,
      QStringLiteral("Bookmark file is unexpectedly large"), error);
  if (!bytes) return false;
  const QString html = QString::fromUtf8(*bytes);
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
    const auto url = NormalizeImportedUrl(DecodeHtml(encoded_url));
    if (!url || seen.contains(*url)) continue;
    seen.insert(*url);
    imported.append(
        Bookmark{*url, NormalizeTitle(DecodeHtml(anchor.captured(2))),
                 QDateTime::currentDateTimeUtc()});
  }
  QList<Bookmark> candidate = bookmarks_;
  for (auto bookmark = imported.crbegin(); bookmark != imported.crend();
       ++bookmark) {
    candidate.prepend(*bookmark);
  }
  if (!BookmarksFitDataBudget(candidate)) {
    SetError(error, QStringLiteral(
                        "Imported bookmarks exceed the profile size limit"));
    return false;
  }
  bookmarks_ = std::move(candidate);
  if (imported_count) *imported_count = imported.size();
  return true;
}

bool BrowsingDataStore::ExportBookmarksHtml(const QString& path,
                                            QString* error) const {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    SetError(error, QStringLiteral("Unable to create export directory"));
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  qint64 bytes_written = 0;
  const auto write_chunk = [&](const QByteArray& bytes) {
    if (bytes.size() > kMaxBookmarkHtmlBytes - bytes_written) {
      SetError(error, QStringLiteral(
                          "Bookmark export exceeds the safe size limit"));
      file.cancelWriting();
      return false;
    }
    if (file.write(bytes) != bytes.size()) {
      SetError(error, file.errorString());
      file.cancelWriting();
      return false;
    }
    bytes_written += bytes.size();
    return true;
  };
  if (!write_chunk(
          QByteArray("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
                     "<META HTTP-EQUIV=\"Content-Type\" "
                     "CONTENT=\"text/html; charset=UTF-8\">\n"
                     "<TITLE>Trail Browser Bookmarks</TITLE>\n"
                     "<H1>Trail Browser Bookmarks</H1>\n<DL><p>\n"))) {
    return false;
  }
  for (const Bookmark& bookmark : bookmarks_) {
    const QString title = bookmark.title.isEmpty() ? bookmark.url
                                                    : bookmark.title;
    const QByteArray entry =
        QStringLiteral(
            "    <DT><A HREF=\"%1\" ADD_DATE=\"%2\">%3</A>\n")
            .arg(bookmark.url.toHtmlEscaped())
            .arg(bookmark.created_at.toSecsSinceEpoch())
            .arg(title.toHtmlEscaped())
            .toUtf8();
    if (!write_chunk(entry)) return false;
  }
  if (!write_chunk(QByteArray("</DL><p>\n"))) return false;
  if (!file.commit()) {
    SetError(error, file.errorString());
    return false;
  }
  return true;
}

bool BrowsingDataStore::IsBookmarked(const QString& url) const {
  const auto normalized = NormalizeRecordableUrl(url);
  if (!normalized) return false;
  return std::any_of(bookmarks_.cbegin(), bookmarks_.cend(),
                     [&normalized](const Bookmark& bookmark) {
                       return bookmark.url == *normalized;
                     });
}

bool BrowsingDataStore::AddBookmark(const QString& url, const QString& title,
                                    QString* error) {
  const auto normalized = NormalizeRecordableUrl(url);
  if (!normalized || IsBookmarked(*normalized) ||
      bookmarks_.size() >= kMaxBookmarks) {
    return false;
  }
  QList<Bookmark> candidate = bookmarks_;
  candidate.prepend(Bookmark{*normalized, NormalizeTitle(title),
                             QDateTime::currentDateTimeUtc()});
  if (!BookmarksFitDataBudget(candidate)) {
    SetError(error, QStringLiteral("Bookmark exceeds the profile size limit"));
    return false;
  }
  bookmarks_ = std::move(candidate);
  return true;
}

bool BrowsingDataStore::RenameBookmark(const QString& url,
                                       const QString& title, QString* error) {
  const auto normalized = NormalizeRecordableUrl(url);
  if (!normalized) return false;
  const auto found = std::find_if(
      bookmarks_.begin(), bookmarks_.end(),
      [&normalized](const Bookmark& bookmark) {
        return bookmark.url == *normalized;
      });
  if (found == bookmarks_.end()) return false;
  const QString previous_title = found->title;
  found->title = NormalizeTitle(title);
  if (!BookmarksFitDataBudget(bookmarks_)) {
    found->title = previous_title;
    SetError(error, QStringLiteral("Bookmark exceeds the profile size limit"));
    return false;
  }
  return true;
}

bool BrowsingDataStore::RemoveBookmark(const QString& url) {
  const auto normalized = NormalizeRecordableUrl(url);
  if (!normalized) return false;
  const auto found = std::find_if(
      bookmarks_.begin(), bookmarks_.end(),
      [&normalized](const Bookmark& bookmark) {
        return bookmark.url == *normalized;
      });
  if (found == bookmarks_.end()) return false;
  bookmarks_.erase(found);
  return true;
}

void BrowsingDataStore::RecordVisit(const QString& url, const QString& title,
                                    QDateTime visited_at) {
  const auto normalized = NormalizeRecordableUrl(url);
  if (!normalized) return;
  const auto found = std::find_if(
      history_.begin(), history_.end(),
      [&normalized](const HistoryEntry& entry) {
        return entry.url == *normalized;
      });
  if (found != history_.end()) {
    HistoryEntry entry = *found;
    history_.erase(found);
    entry.title = NormalizeTitle(title);
    entry.last_visited_at = NormalizeDate(visited_at);
    if (entry.visit_count < std::numeric_limits<int>::max()) {
      ++entry.visit_count;
    }
    history_.prepend(std::move(entry));
  } else {
    history_.prepend(HistoryEntry{*normalized, NormalizeTitle(title),
                                  NormalizeDate(visited_at), 1});
  }
  while (history_.size() > kMaxHistoryEntries) history_.removeLast();
}

bool BrowsingDataStore::RemoveHistory(const QString& url) {
  const auto normalized = NormalizeRecordableUrl(url);
  if (!normalized) return false;
  const auto found = std::find_if(
      history_.begin(), history_.end(),
      [&normalized](const HistoryEntry& entry) {
        return entry.url == *normalized;
      });
  if (found == history_.end()) return false;
  history_.erase(found);
  return true;
}

void BrowsingDataStore::ClearHistory() {
  history_.clear();
}

qsizetype BrowsingDataStore::BookmarkBytesForTesting() const {
  return SerializeData(SerializeBookmarks(bookmarks_), {}).size();
}

qsizetype BrowsingDataStore::MaxDataBytesForTesting() {
  return kMaxDataBytes;
}

bool BrowsingDataStore::IsRecordableUrl(const QString& url) {
  return NormalizeRecordableUrl(url).has_value();
}
