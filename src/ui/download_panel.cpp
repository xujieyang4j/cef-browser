#include "ui/download_panel.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "download/download_manager.h"

namespace {
constexpr int kActionColumn = 3;
}

DownloadPanel::DownloadPanel(DownloadManager* manager, QWidget* parent)
    : QFrame(parent), manager_(manager) {
  setFrameShape(QFrame::StyledPanel);
  setMaximumHeight(230);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 6, 8, 8);
  layout->setSpacing(4);
  auto* header = new QHBoxLayout();
  title_ = new QLabel(QStringLiteral("Downloads"), this);
  clear_button_ = new QPushButton(QStringLiteral("Clear finished"), this);
  auto* hide_button = new QPushButton(QStringLiteral("Hide"), this);
  header->addWidget(title_);
  header->addStretch();
  header->addWidget(clear_button_);
  header->addWidget(hide_button);
  layout->addLayout(header);

  list_ = new QTreeWidget(this);
  list_->setColumnCount(4);
  list_->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Status"),
                          QStringLiteral("Size"), QStringLiteral("Actions")});
  list_->setRootIsDecorated(false);
  list_->setAlternatingRowColors(true);
  list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  list_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  layout->addWidget(list_);

  connect(hide_button, &QPushButton::clicked, this, &QWidget::hide);
  connect(clear_button_, &QPushButton::clicked, manager_,
          &DownloadManager::ClearFinished);
  connect(manager_, &DownloadManager::DownloadChanged, this,
          [this](quint32 id, bool is_new) { RefreshDownload(id, is_new); });
  connect(manager_, &DownloadManager::DownloadRemoved, this,
          [this](quint32 id) { RemoveDownload(id); });
  connect(manager_, &DownloadManager::ActiveCountChanged, this,
          [this](int) { UpdateHeader(); });
  const QList<DownloadManager::Item> restored = manager_->items();
  for (auto item = restored.crbegin(); item != restored.crend(); ++item) {
    RefreshDownload(item->id, false);
  }
  hide();
}

void DownloadPanel::ToggleVisibility() {
  setVisible(!isVisible());
}

void DownloadPanel::RefreshDownload(quint32 id, bool is_new) {
  const auto download = manager_->item(id);
  if (!download) return;

  QTreeWidgetItem* row = rows_.value(id);
  if (!row) {
    row = new QTreeWidgetItem();
    rows_.insert(id, row);
    list_->insertTopLevelItem(0, row);

    auto* actions = new QWidget(list_);
    auto* action_layout = new QHBoxLayout(actions);
    action_layout->setContentsMargins(0, 0, 0, 0);
    action_layout->setSpacing(4);
    auto* open_button = new QPushButton(QStringLiteral("Open"), actions);
    auto* folder_button = new QPushButton(QStringLiteral("Folder"), actions);
    auto* cancel_button = new QPushButton(QStringLiteral("Cancel"), actions);
    auto* pause_button = new QPushButton(QStringLiteral("Pause"), actions);
    auto* remove_button = new QPushButton(QStringLiteral("Remove"), actions);
    open_button->setObjectName(QStringLiteral("openDownload"));
    folder_button->setObjectName(QStringLiteral("showDownload"));
    cancel_button->setObjectName(QStringLiteral("cancelDownload"));
    pause_button->setObjectName(QStringLiteral("pauseDownload"));
    remove_button->setObjectName(QStringLiteral("removeDownload"));
    action_layout->addWidget(open_button);
    action_layout->addWidget(folder_button);
    action_layout->addWidget(pause_button);
    action_layout->addWidget(cancel_button);
    action_layout->addWidget(remove_button);
    list_->setItemWidget(row, kActionColumn, actions);
    connect(open_button, &QPushButton::clicked, this,
            [this, id] { manager_->OpenDownload(id); });
    connect(folder_button, &QPushButton::clicked, this,
            [this, id] { manager_->ShowDownloadInFolder(id); });
    connect(cancel_button, &QPushButton::clicked, this,
            [this, id] { manager_->CancelDownload(id); });
    connect(pause_button, &QPushButton::clicked, this, [this, id] {
      const auto download = manager_->item(id);
      if (!download) return;
      if (download->state == DownloadManager::State::Paused) {
        manager_->ResumeDownload(id);
      } else {
        manager_->PauseDownload(id);
      }
    });
    connect(remove_button, &QPushButton::clicked, this,
            [this, id] { manager_->RemoveDownload(id); });
  }

  row->setText(0, download->file_name.isEmpty() ? download->url
                                                 : download->file_name);
  row->setToolTip(0, download->full_path.isEmpty() ? download->url
                                                    : download->full_path);
  row->setText(1, DownloadManager::StatusText(*download));
  row->setText(2, download->total_bytes > 0
                      ? QStringLiteral("%1 / %2")
                            .arg(DownloadManager::FormatBytes(download->received_bytes),
                                 DownloadManager::FormatBytes(download->total_bytes))
                      : DownloadManager::FormatBytes(download->received_bytes));

  if (QWidget* actions = list_->itemWidget(row, kActionColumn)) {
    const bool active = download->state == DownloadManager::State::Starting ||
                        download->state == DownloadManager::State::InProgress ||
                        download->state == DownloadManager::State::Paused;
    const bool complete = download->state == DownloadManager::State::Complete;
    actions->findChild<QPushButton*>(QStringLiteral("openDownload"))
        ->setVisible(complete && !download->full_path.isEmpty());
    actions->findChild<QPushButton*>(QStringLiteral("showDownload"))
        ->setVisible(!active && !download->full_path.isEmpty());
    actions->findChild<QPushButton*>(QStringLiteral("cancelDownload"))
        ->setVisible(active);
    actions->findChild<QPushButton*>(QStringLiteral("removeDownload"))
        ->setVisible(!active);
    QPushButton* pause =
        actions->findChild<QPushButton*>(QStringLiteral("pauseDownload"));
    pause->setVisible(active &&
                      download->state != DownloadManager::State::Starting);
    pause->setText(download->state == DownloadManager::State::Paused
                       ? QStringLiteral("Resume")
                       : QStringLiteral("Pause"));
  }
  if (is_new) show();
  UpdateHeader();
}

void DownloadPanel::RemoveDownload(quint32 id) {
  QTreeWidgetItem* row = rows_.take(id);
  delete row;
  UpdateHeader();
}

void DownloadPanel::UpdateHeader() {
  const int active = manager_->active_count();
  title_->setText(active > 0 ? QStringLiteral("Downloads (%1 active)").arg(active)
                             : QStringLiteral("Downloads"));
  clear_button_->setEnabled(manager_->items().size() > active);
}
