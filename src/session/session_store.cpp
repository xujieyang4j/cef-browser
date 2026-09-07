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

constexpr int kSessionVersion = 1;
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
  if (root.value(QStringLiteral("version")).toInt() != kSessionVersion) {
    SetError(error, QStringLiteral("Unsupported session version"));
    return std::nullopt;
  }

  BrowserSession session;
  const QJsonArray tabs = root.value(QStringLiteral("tabs")).toArray();
  const int tab_count =
      std::min(static_cast<int>(tabs.size()), kMaxRestoredTabs);
  for (int index = 0; index < tab_count; ++index) {
    const QString url = tabs.at(index).toObject()
                            .value(QStringLiteral("url"))
                            .toString()
                            .trimmed();
    if (!url.isEmpty()) session.tab_urls.append(url);
  }
  if (session.tab_urls.isEmpty()) {
    SetError(error, QStringLiteral("Session contains no restorable tabs"));
    return std::nullopt;
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
  for (const QString& url : session.tab_urls.mid(0, kMaxRestoredTabs)) {
    if (!url.trimmed().isEmpty()) {
      tabs.append(QJsonObject{{QStringLiteral("url"), url}});
    }
  }
  if (tabs.isEmpty()) {
    SetError(error, QStringLiteral("Refusing to save an empty session"));
    return false;
  }

  const QJsonObject root{
      {QStringLiteral("version"), kSessionVersion},
      {QStringLiteral("cleanExit"), session.clean_exit},
      {QStringLiteral("activeTab"),
       std::clamp(session.active_tab, 0, static_cast<int>(tabs.size()) - 1)},
      {QStringLiteral("windowGeometry"),
       QString::fromLatin1(session.window_geometry.toBase64())},
      {QStringLiteral("tabs"), tabs},
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
