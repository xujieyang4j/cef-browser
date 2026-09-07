#include "util/corrupt_file.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace trail {
namespace {

constexpr int kMaxCorruptBackups = 5;

void PruneCorruptBackups(const QFileInfo& source,
                         const QString& newest_backup) {
  QDir directory = source.absoluteDir();
  const QString prefix = source.fileName() + QStringLiteral(".corrupt-");
  const QString pattern = prefix + QLatin1Char('*');
  const QString newest_absolute = QFileInfo(newest_backup).absoluteFilePath();
  QFileInfoList backups = directory.entryInfoList(
      {pattern}, QDir::Files, QDir::Name);
  backups.erase(
      std::remove_if(backups.begin(), backups.end(),
                     [&newest_absolute](const QFileInfo& backup) {
                       return backup.absoluteFilePath() == newest_absolute;
                     }),
      backups.end());
  const auto backup_sequence = [&prefix](const QFileInfo& backup) {
    QString value = backup.fileName().mid(prefix.size());
    const qsizetype separator = value.lastIndexOf(QLatin1Char('-'));
    int suffix = 0;
    if (separator >= 0) {
      bool suffix_ok = false;
      const int candidate = value.mid(separator + 1).toInt(&suffix_ok);
      if (suffix_ok) {
        value.truncate(separator);
        suffix = candidate;
      }
    }
    return std::pair<QString, int>{value, suffix};
  };
  std::sort(backups.begin(), backups.end(),
            [&backup_sequence](const QFileInfo& left, const QFileInfo& right) {
              return backup_sequence(left) < backup_sequence(right);
            });
  const int remove_count = std::max(
      0, static_cast<int>(backups.size()) - (kMaxCorruptBackups - 1));
  for (int index = 0; index < remove_count; ++index) {
    // The current damaged file is already safe at newest_backup. Retention
    // cleanup is best effort so an undeletable older backup never turns a
    // successful preservation into a failure.
    QFile::remove(backups.at(index).absoluteFilePath());
  }
}

}  // namespace

bool PreserveCorruptFile(const QString& path, QString* preserved_path,
                         QString* error) {
  if (preserved_path) preserved_path->clear();
  if (error) error->clear();
  const QFileInfo source(path);
  if (path.isEmpty() || !source.exists()) {
    if (error) *error = QStringLiteral("Unreadable profile file is missing");
    return false;
  }
  const QString timestamp = QDateTime::currentDateTimeUtc().toString(
      QStringLiteral("yyyyMMddTHHmmsszzzZ"));
  const QString base = path + QStringLiteral(".corrupt-") + timestamp;
  for (int suffix = 0; suffix < 1000; ++suffix) {
    const QString candidate =
        suffix == 0 ? base : base + QStringLiteral("-%1").arg(suffix);
    if (QFileInfo::exists(candidate)) continue;
    QFile file(path);
    if (file.rename(candidate)) {
      if (preserved_path) *preserved_path = candidate;
      PruneCorruptBackups(source, candidate);
      return true;
    }
    if (error) *error = file.errorString();
    return false;
  }

  if (error) {
    *error = QStringLiteral("Unable to allocate a corrupt-file backup name");
  }
  return false;
}

int MaxCorruptBackupsForTesting() {
  return kMaxCorruptBackups;
}

}  // namespace trail
