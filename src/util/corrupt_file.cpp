#include "util/corrupt_file.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>

namespace trail {

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

}  // namespace trail
