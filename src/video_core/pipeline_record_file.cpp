// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "video_core/pipeline_record_file.h"

namespace VideoCommon {

PipelineRecordFile::PipelineRecordFile(const std::filesystem::path& path) {
#ifdef _WIN32
    handle = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
    fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
}

PipelineRecordFile::~PipelineRecordFile() {
#ifdef _WIN32
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
#else
    if (fd >= 0) {
        close(fd);
    }
#endif
}

bool PipelineRecordFile::IsOpen() const noexcept {
#ifdef _WIN32
    return handle != INVALID_HANDLE_VALUE;
#else
    return fd >= 0;
#endif
}

bool PipelineRecordFile::ReadAt(u64 offset, std::span<char> out) const {
    while (!out.empty()) {
#ifdef _WIN32
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        const DWORD wanted = static_cast<DWORD>((std::min<size_t>)(out.size(), size_t{1} << 30));
        DWORD read = 0;
        if (!ReadFile(handle, out.data(), wanted, &read, &position) || read == 0) {
            return false;
        }
#else
        const ssize_t read = pread(fd, out.data(), out.size(), static_cast<off_t>(offset));
        if (read <= 0) {
            return false;
        }
#endif
        offset += static_cast<u64>(read);
        out = out.subspan(static_cast<size_t>(read));
    }
    return true;
}

} // namespace VideoCommon
