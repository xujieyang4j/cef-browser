#pragma once

#include <QString>

namespace trail {

// Moves an unreadable profile file aside in the same directory before a new
// file may be written at its original path. The source must already be closed.
bool PreserveCorruptFile(const QString& path, QString* preserved_path = nullptr,
                         QString* error = nullptr);

}  // namespace trail
