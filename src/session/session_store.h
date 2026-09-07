#pragma once

#include <optional>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

struct RecentlyClosedTab {
  QString url;
  QString title;

  bool operator==(const RecentlyClosedTab&) const = default;
};

struct BrowserSession {
  QStringList tab_urls;
  QList<bool> tab_pinned;
  QList<RecentlyClosedTab> recently_closed_tabs;
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
  static bool MarkLaunchStarted(const QString& path,
                                const BrowserSession& restored_session,
                                QString* error = nullptr);

 private:
  SessionStore() = delete;
};
