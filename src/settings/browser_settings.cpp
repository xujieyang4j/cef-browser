#include "settings/browser_settings.h"

#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

namespace {

constexpr int kSettingsVersion = 1;
constexpr int kMaxSettingsBytes = 64 * 1024;

void SetError(QString* error, const QString& value) {
  if (error) *error = value;
}

}  // namespace

BrowserSettings::BrowserSettings(QString path) : path_(std::move(path)) {}

bool BrowserSettings::Load(QString* error) {
  QFile file(path_);
  if (!file.exists()) return true;
  if (!file.open(QIODevice::ReadOnly)) {
    SetError(error, file.errorString());
    return false;
  }
  if (file.size() > kMaxSettingsBytes) {
    SetError(error, QStringLiteral("Settings file is unexpectedly large"));
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
  if (root.value(QStringLiteral("version")).toInt() != kSettingsVersion) {
    SetError(error, QStringLiteral("Unsupported settings version"));
    return false;
  }
  const QString id = root.value(QStringLiteral("searchEngine")).toString();
  if (id == QStringLiteral("duckduckgo")) {
    search_engine_ = SearchEngine::DuckDuckGo;
  } else if (id == QStringLiteral("bing")) {
    search_engine_ = SearchEngine::Bing;
  } else {
    search_engine_ = SearchEngine::Google;
  }
  set_home_page(root.value(QStringLiteral("homePage")).toString());
  return true;
}

bool BrowserSettings::Save(QString* error) const {
  if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
    SetError(error, QStringLiteral("Unable to create settings directory"));
    return false;
  }
  const QJsonObject root{
      {QStringLiteral("version"), kSettingsVersion},
      {QStringLiteral("searchEngine"), SearchEngineId(search_engine_)},
      {QStringLiteral("homePage"), home_page_},
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

bool BrowserSettings::set_home_page(const QString& url) {
  const QString trimmed = url.trimmed();
  if (trimmed.isEmpty()) return false;
  if (trimmed == QStringLiteral("about:blank")) {
    home_page_ = trimmed;
    return true;
  }
  const QUrl parsed(trimmed);
  const QString scheme = parsed.scheme().toLower();
  if (!parsed.isValid() || parsed.host().isEmpty() ||
      (scheme != QStringLiteral("http") &&
       scheme != QStringLiteral("https"))) {
    return false;
  }
  home_page_ = parsed.toString();
  return true;
}

QString BrowserSettings::SearchEngineId(SearchEngine engine) {
  switch (engine) {
    case SearchEngine::Google:
      return QStringLiteral("google");
    case SearchEngine::DuckDuckGo:
      return QStringLiteral("duckduckgo");
    case SearchEngine::Bing:
      return QStringLiteral("bing");
  }
  return QStringLiteral("google");
}

QString BrowserSettings::SearchEngineName(SearchEngine engine) {
  switch (engine) {
    case SearchEngine::Google:
      return QStringLiteral("Google");
    case SearchEngine::DuckDuckGo:
      return QStringLiteral("DuckDuckGo");
    case SearchEngine::Bing:
      return QStringLiteral("Bing");
  }
  return QStringLiteral("Google");
}

QString BrowserSettings::SearchUrl(SearchEngine engine, const QString& query) {
  const QString encoded =
      QString::fromLatin1(QUrl::toPercentEncoding(query.trimmed()));
  switch (engine) {
    case SearchEngine::Google:
      return QStringLiteral("https://www.google.com/search?q=%1").arg(encoded);
    case SearchEngine::DuckDuckGo:
      return QStringLiteral("https://duckduckgo.com/?q=%1").arg(encoded);
    case SearchEngine::Bing:
      return QStringLiteral("https://www.bing.com/search?q=%1").arg(encoded);
  }
  return QStringLiteral("https://www.google.com/search?q=%1").arg(encoded);
}
