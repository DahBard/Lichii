// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <span>

#include "common/common_types.h"

namespace VideoCommon {

/// The pipeline disk cache, read at record offsets (EDEN_NX2_PREBUILD_STREAM: records read again
/// just before their build). Opened before the loader reads the file and shared for writing and
/// deleting: the serialization thread appends records meanwhile (appends never move a record),
/// and a rewrite that replaces the file leaves this handle reading the records found before.
class PipelineRecordFile {
public:
    explicit PipelineRecordFile(const std::filesystem::path& path);
    ~PipelineRecordFile();

    PipelineRecordFile(const PipelineRecordFile&) = delete;
    PipelineRecordFile& operator=(const PipelineRecordFile&) = delete;

    [[nodiscard]] bool IsOpen() const noexcept;

    /// Reads out.size() bytes at offset; false when the file ends first or the read fails.
    [[nodiscard]] bool ReadAt(u64 offset, std::span<char> out) const;

private:
#ifdef _WIN32
    void* handle;
#else
    int fd{-1};
#endif
};

} // namespace VideoCommon
