#include "util/bounded_file.h"

#include <QFile>

namespace trail {

std::optional<QByteArray> ReadBoundedFile(QFile& file, qint64 max_bytes,
                                          const QString& oversized_error,
                                          QString* error) {
  const QByteArray bytes = file.read(max_bytes + 1);
  if (file.error() != QFileDevice::NoError) {
    if (error) *error = file.errorString();
    return std::nullopt;
  }
  if (bytes.size() > max_bytes || !file.atEnd()) {
    if (error) *error = oversized_error;
    return std::nullopt;
  }
  return bytes;
}

}  // namespace trail
