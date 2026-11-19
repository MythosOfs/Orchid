// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "core/file_sys/vfs/vfs_types.h"

namespace FileSys {
/// A read-only PFS0 (NSP layout) holding `files` in order, without copying their bytes.
VirtualFile BuildPartitionFile(std::string name, const std::vector<VirtualFile>& files);

} // namespace FileSys
