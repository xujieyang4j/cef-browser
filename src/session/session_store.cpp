#include "session/session_store.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr int kSessionVersion = 2;
constexpr int kLegacySessionVersion = 1;
constexpr int kMaxRestoredTabs = 100;
constexpr int kMaxSessionBytes = 1024 * 1024;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
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
  const int tab_count =
      std::min(static_cast<int>(tabs.size()), kMaxRestoredTabs);
  for (int index = 0; index < tab_count; ++index) {
    const QJsonObject tab = tabs.at(index).toObject();
    const QString url =
        tab.value(QStringLiteral("url")).toString().trimmed();
    if (!url.isEmpty()) {
      session.tab_urls.append(url);
      session.tab_pinned.append(
          tab.value(QStringLiteral("pinned")).toBool(false));
    }
  }
  if (session.tab_urls.isEmpty()) {
    SetError(error, QStringLiteral("Session contains no restorable tabs"));
    return std::nullopt;
  }
  const QJsonArray recently_closed =
      root.value(QStringLiteral("recentlyClosed")).toArray();
  const int recently_closed_count =
      std::min(static_cast<int>(recently_closed.size()), kMaxRestoredTabs);
  for (int index = 0; index < recently_closed_count; ++index) {
    const QJsonValue value = recently_closed.at(index);
    const QJsonObject object = value.toObject();
    const QString url =
        (value.isString() ? value.toString()
                          : object.value(QStringLiteral("url")).toString())
            .trimmed();
    const QString title =
        object.value(QStringLiteral("title")).toString().trimmed();
    if (!url.isEmpty()) {
      session.recently_closed_tabs.append(RecentlyClosedTab{url, title});
    }
  }

  session.active_tab = std::clamp(
      root.value(QStringLiteral("activeTab")).toInt(), 0,
      static_cast<int>(session.tab_urls.size()) - 1);
  session.clean_exit = root.value(QStringLiteral("cleanExit")).toBool(false);
  session.window_geometry = QByteArray::fromBase64(
      root.value(QStringLiteral("windowGeometry")).toString().toLatin1());
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
  for (int index = 0; index < tab_count; ++index) {
    const QString& url = session.tab_urls.at(index);
    if (!url.trimmed().isEmpty()) {
      const bool pinned = index < session.tab_pinned.size() &&
                          session.tab_pinned.at(index);
      tabs.append(QJsonObject{{QStringLiteral("url"), url},
                              {QStringLiteral("pinned"), pinned}});
    }
  }
  if (tabs.isEmpty()) {
    SetError(error, QStringLiteral("Refusing to save an empty session"));
    return false;
  }
  QJsonArray recently_closed;
  for (const RecentlyClosedTab& tab :
       session.recently_closed_tabs.mid(0, kMaxRestoredTabs)) {
    if (!tab.url.trimmed().isEmpty()) {
      recently_closed.append(
          QJsonObject{{QStringLiteral("url"), tab.url},
                      {QStringLiteral("title"), tab.title}});
    }
  }

  const QJsonObject root{
      {QStringLiteral("version"), kSessionVersion},
      {QStringLiteral("cleanExit"), session.clean_exit},
      {QStringLiteral("activeTab"),
       std::clamp(session.active_tab, 0, static_cast<int>(tabs.size()) - 1)},
      {QStringLiteral("windowGeometry"),
       QString::fromLatin1(session.window_geometry.toBase64())},
      {QStringLiteral("tabs"), tabs},
      {QStringLiteral("recentlyClosed"), recently_closed},
  };

  QSaveFile file(path);
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
