#pragma once

#include <QFrame>
#include <QHash>

class DownloadManager;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class DownloadPanel final : public QFrame {
 public:
  explicit DownloadPanel(DownloadManager* manager, QWidget* parent = nullptr);

  void ToggleVisibility();

 private:
  void RefreshDownload(quint32 id, bool is_new);
  void RemoveDownload(quint32 id);
  void UpdateHeader();

  DownloadManager* manager_ = nullptr;
  QLabel* title_ = nullptr;
  QPushButton* clear_button_ = nullptr;
  QTreeWidget* list_ = nullptr;
  QHash<quint32, QTreeWidgetItem*> rows_;
};
