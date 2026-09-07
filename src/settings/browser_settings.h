#pragma once

#include <QString>

class BrowserSettings final {
 public:
  enum class SearchEngine { Google, DuckDuckGo, Bing };

  explicit BrowserSettings(QString path);

  bool Load(QString* error = nullptr);
  bool Save(QString* error = nullptr) const;
  SearchEngine search_engine() const { return search_engine_; }
  void set_search_engine(SearchEngine engine) { search_engine_ = engine; }
  const QString& home_page() const { return home_page_; }
  bool set_home_page(const QString& url);
  bool open_home_on_new_tab() const { return open_home_on_new_tab_; }
  void set_open_home_on_new_tab(bool enabled) {
    open_home_on_new_tab_ = enabled;
  }

  static QString SearchEngineId(SearchEngine engine);
  static QString SearchEngineName(SearchEngine engine);
  static QString SearchUrl(SearchEngine engine, const QString& query);

 private:
  QString path_;
  SearchEngine search_engine_ = SearchEngine::Google;
  QString home_page_ = QStringLiteral("https://www.example.com");
  bool open_home_on_new_tab_ = false;
};
