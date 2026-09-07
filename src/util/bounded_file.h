#pragma once

#include <optional>

#include <QByteArray>
#include <QString>

class QFile;

namespace trail {

// Reads at most max_bytes plus one sentinel byte so the limit remains valid
// even if the file grows after metadata was inspected. The file must already
// be open for reading.
std::optional<QByteArray> ReadBoundedFile(QFile& file, qint64 max_bytes,
                                          const QString& oversized_error,
                                          QString* error = nullptr);

}  // namespace trail
