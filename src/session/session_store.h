#pragma once

#include <optional>

#include <QByteArray>
#include <QString>
#include <QStringList>

struct BrowserSession {
  QStringList tab_urls;
  QStringList recently_closed_urls;
  int active_tab = 0;
  QByteArray window_geometry;
  bool clean_exit = true;
};

class SessionStore final {
 public:
  static std::optional<BrowserSession> Load(const QString& path,
                                            QString* error = nullptr);
  static bool Save(const QString& path, const BrowserSession& session,
                   QString* error = nullptr);

 private:
  SessionStore() = delete;
};
