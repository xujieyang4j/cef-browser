#include "session/session_store.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "settings/browser_settings.h"

namespace {

constexpr int kSessionVersion = 2;
constexpr int kLegacySessionVersion = 1;
constexpr int kMaxRestoredTabs = 100;
constexpr int kMaxSessionBytes = 1024 * 1024;
constexpr int kMaxSessionUrlBytes = 64 * 1024;
constexpr int kMaxClosedTitleCharacters = 512;
constexpr int kMaxWindowGeometryBytes = 64 * 1024;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
}

std::optional<QString> NormalizeSessionUrl(QString value) {
  const auto normalized =
      BrowserSettings::NormalizeStoredUrl(std::move(value));
  if (!normalized || normalized->toUtf8().size() > kMaxSessionUrlBytes) {
    return std::nullopt;
  }
  return normalized;
}

QString NormalizeClosedTitle(QString value) {
  for (qsizetype index = 0; index < value.size(); ++index) {
    if (value.at(index).category() == QChar::Other_Control) {
      value[index] = QLatin1Char(' ');
    }
  }
  return value.simplified().left(kMaxClosedTitleCharacters);
}

}  // namespace

std::optional<BrowserSession> SessionStore::Load(const QString& path,
                                                 QString* error) {
  QFile file(path);
  if (!file.exists()) return std::nullopt;
  if (!file.open(QIODevice::ReadOnly)) {
    SetError(error, file.errorString());
    return std::nullopt;
  }
  if (file.size() > kMaxSessionBytes) {
    SetError(error, QStringLiteral("Session file is unexpectedly large"));
    return std::nullopt;
  }

  QJsonParseError parse_error;
  const QJsonDocument document =
      QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    SetError(error, parse_error.errorString());
    return std::nullopt;
  }

  const QJsonObject root = document.object();
  const int version = root.value(QStringLiteral("version")).toInt();
  if (version != kSessionVersion && version != kLegacySessionVersion) {
    SetError(error, QStringLiteral("Unsupported session version"));
    return std::nullopt;
  }

  BrowserSession session;
  const QJsonArray tabs = root.value(QStringLiteral("tabs")).toArray();
  const int tab_count = tabs.size();
  const int requested_active_tab =
      std::clamp(root.value(QStringLiteral("activeTab")).toInt(), 0,
                 std::max(0, tab_count - 1));
  int restored_active_tab = -1;
  int restored_active_distance = tab_count + 1;
  for (int index = 0; index < tab_count &&
                      session.tab_urls.size() < kMaxRestoredTabs;
       ++index) {
    const QJsonObject tab = tabs.at(index).toObject();
    const auto url = NormalizeSessionUrl(
        tab.value(QStringLiteral("url")).toString());
    if (!url) continue;
    const int restored_index = session.tab_urls.size();
    const int distance = std::abs(index - requested_active_tab);
    if (distance < restored_active_distance ||
        (distance == restored_active_distance &&
         index >= requested_active_tab)) {
      restored_active_tab = restored_index;
      restored_active_distance = distance;
    }
    session.tab_urls.append(*url);
    session.tab_pinned.append(
        tab.value(QStringLiteral("pinned")).toBool(false));
  }
  if (session.tab_urls.isEmpty()) {
    SetError(error, QStringLiteral("Session contains no restorable tabs"));
    return std::nullopt;
  }
  const QJsonArray recently_closed =
      root.value(QStringLiteral("recentlyClosed")).toArray();
  for (int index = 0; index < recently_closed.size() &&
                      session.recently_closed_tabs.size() < kMaxRestoredTabs;
       ++index) {
    const QJsonValue value = recently_closed.at(index);
    const QJsonObject object = value.toObject();
    const QString raw_url =
        (value.isString() ? value.toString()
                          : object.value(QStringLiteral("url")).toString())
            .trimmed();
    const auto url = NormalizeSessionUrl(raw_url);
    const QString title = NormalizeClosedTitle(
        object.value(QStringLiteral("title")).toString());
    if (url) {
      session.recently_closed_tabs.append(RecentlyClosedTab{*url, title});
    }
  }

  session.active_tab = std::max(0, restored_active_tab);
  session.clean_exit = root.value(QStringLiteral("cleanExit")).toBool(false);
  session.window_geometry = QByteArray::fromBase64(
      root.value(QStringLiteral("windowGeometry")).toString().toLatin1());
  if (session.window_geometry.size() > kMaxWindowGeometryBytes) {
    session.window_geometry.clear();
  }
  return session;
}

bool SessionStore::Save(const QString& path, const BrowserSession& session,
                        QString* error) {
  if (session.tab_urls.isEmpty()) {
    SetError(error, QStringLiteral("Refusing to save an empty session"));
    return false;
  }
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    SetError(error, QStringLiteral("Unable to create session directory"));
    return false;
  }

  QJsonArray tabs;
  const int tab_count = std::min(
      static_cast<int>(session.tab_urls.size()), kMaxRestoredTabs);
  const int requested_active_tab =
      std::clamp(session.active_tab, 0, std::max(0, tab_count - 1));
  int saved_active_tab = -1;
  int saved_active_distance = tab_count + 1;
  for (int index = 0; index < tab_count; ++index) {
    const auto url = NormalizeSessionUrl(session.tab_urls.at(index));
    if (!url) continue;
    const int candidate_index = tabs.size();
    const int distance = std::abs(index - requested_active_tab);
    if (distance < saved_active_distance ||
        (distance == saved_active_distance && index >= requested_active_tab)) {
      saved_active_tab = candidate_index;
      saved_active_distance = distance;
    }
    const bool pinned = index < session.tab_pinned.size() &&
                        session.tab_pinned.at(index);
    tabs.append(QJsonObject{{QStringLiteral("url"), *url},
                            {QStringLiteral("pinned"), pinned}});
  }
  if (tabs.isEmpty()) {
    SetError(error, QStringLiteral("Refusing to save an empty session"));
    return false;
  }
  QJsonArray recently_closed;
  for (const RecentlyClosedTab& tab :
       session.recently_closed_tabs.mid(0, kMaxRestoredTabs)) {
    const auto url = NormalizeSessionUrl(tab.url);
    if (url) {
      recently_closed.append(
          QJsonObject{{QStringLiteral("url"), *url},
                      {QStringLiteral("title"),
                       NormalizeClosedTitle(tab.title)}});
    }
  }
  const QByteArray window_geometry =
      session.window_geometry.size() <= kMaxWindowGeometryBytes
          ? session.window_geometry
          : QByteArray();
  const auto make_root = [&] {
    return QJsonObject{
        {QStringLiteral("version"), kSessionVersion},
        {QStringLiteral("cleanExit"), session.clean_exit},
        {QStringLiteral("activeTab"), saved_active_tab},
        {QStringLiteral("windowGeometry"),
         QString::fromLatin1(window_geometry.toBase64())},
        {QStringLiteral("tabs"), tabs},
        {QStringLiteral("recentlyClosed"), recently_closed},
    };
  };
  QByteArray bytes = QJsonDocument(make_root()).toJson(QJsonDocument::Compact);
  while (bytes.size() > kMaxSessionBytes && !recently_closed.isEmpty()) {
    recently_closed.removeLast();
    bytes = QJsonDocument(make_root()).toJson(QJsonDocument::Compact);
  }
  while (bytes.size() > kMaxSessionBytes && tabs.size() > 1) {
    const int last_index = tabs.size() - 1;
    const int remove_index =
        saved_active_tab <= last_index / 2 ? last_index : 0;
    tabs.removeAt(remove_index);
    if (remove_index < saved_active_tab) --saved_active_tab;
    bytes = QJsonDocument(make_root()).toJson(QJsonDocument::Compact);
  }
  if (bytes.size() > kMaxSessionBytes) {
    SetError(error, QStringLiteral("Session data exceeds the safe size limit"));
    return false;
  }

  QSaveFile file(path);
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
