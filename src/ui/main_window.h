#pragma once

#include <QMainWindow>

class BrowserView;
class QCloseEvent;
class QLineEdit;
class QPushButton;

class MainWindow final : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(const QString& initial_url, QWidget* parent = nullptr);

 protected:
  void closeEvent(QCloseEvent* event) override;

 private slots:
  void NavigateFromAddressBar();
  void UpdateLoadingState(bool loading, bool can_go_back,
                          bool can_go_forward);
  void UpdateAddress(const QString& url);

 private:
  static QString NormalizeUrl(QString input);

  BrowserView* browser_view_ = nullptr;
  QLineEdit* address_bar_ = nullptr;
  QPushButton* back_button_ = nullptr;
  QPushButton* forward_button_ = nullptr;
  QPushButton* reload_button_ = nullptr;
  bool close_requested_ = false;

  Q_DISABLE_COPY_MOVE(MainWindow)
};

