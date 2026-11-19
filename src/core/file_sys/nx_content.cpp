// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "common/alignment.h"
#include "core/file_sys/nx_content.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_concat.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {

namespace {

template <typename T>
void Put(std::vector<u8>& out, std::size_t at, T value) {
    std::memcpy(out.data() + at, &value, sizeof(T));
}

} // namespace

VirtualFile BuildPartitionFile(std::string name, const std::vector<VirtualFile>& files) {
    std::vector<u8> strings;
    for (const auto& file : files) {
        const auto file_name = file->GetName();
        strings.insert(strings.end(), file_name.begin(), file_name.end());
        strings.push_back(0);
    }
    constexpr std::size_t PfsHeaderSize = 0x10;
    constexpr std::size_t EntrySize = 0x18;
    const std::size_t fixed = PfsHeaderSize + EntrySize * files.size();
    strings.resize(Common::AlignUp(fixed + strings.size(), 0x20) - fixed);

    std::vector<u8> header(fixed + strings.size());
    std::memcpy(header.data(), "PFS0", 4);
    Put<u32>(header, 4, static_cast<u32>(files.size()));
    Put<u32>(header, 8, static_cast<u32>(strings.size()));

    u64 offset = 0;
    u32 name_offset = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto at = PfsHeaderSize + EntrySize * i;
        const u64 size = files[i]->GetSize();
        Put<u64>(header, at, offset);
        Put<u64>(header, at + 8, size);
        Put<u32>(header, at + 16, name_offset);
        offset += size;
        name_offset += static_cast<u32>(files[i]->GetName().size() + 1);
    }
    if (!strings.empty()) {
        std::memcpy(header.data() + fixed, strings.data(), strings.size());
    }

    std::vector<VirtualFile> parts;
    parts.reserve(files.size() + 1);
    parts.push_back(std::make_shared<VectorVfsFile>(std::move(header), "header"));
    for (const auto& file : files) {
        if (file->GetSize() != 0) {
            parts.push_back(file);
        }
    }
    return ConcatenatedVfsFile::MakeConcatenatedFile(std::move(name), std::move(parts));
}

} // namespace FileSys
