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

  static QString SearchEngineId(SearchEngine engine);
  static QString SearchEngineName(SearchEngine engine);
  static QString SearchUrl(SearchEngine engine, const QString& query);

 private:
  QString path_;
  SearchEngine search_engine_ = SearchEngine::Google;
};
