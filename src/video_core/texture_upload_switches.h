// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon {

/// Cheaper texture uploads on the GPU thread, from Fritchii v4 (Frizti/Lichii 7d75f9133, its
/// EDEN_NX2_EXP8, which this reads too; Ocarina walking through Hyrule Field there: 1% low
/// 30.3 -> 35.1 fps). LICHII_TEXTURE_UPLOADS bits, all on by default (15), 0 = as before:
///  1  (no longer a switch: upstream #778 keeps idle upload staging buffers always)
///  2  no guest upload for a new single-level 2D image that GPU copies overwrite whole
///  4  2D levels of 2 MiB or more untiled in bands on the transcode workers too
///  8  guest memory of uploaded textures not hinted cold (it faulted back in on re-uploads)
inline int TextureUploadBits() {
    static const int bits = [] {
        for (const char* name : {"LICHII_TEXTURE_UPLOADS", "EDEN_NX2_EXP8"}) {
            const char* value = std::getenv(name);
            if (value != nullptr && value[0] != '\0') {
                return std::atoi(value);
            }
        }
        return 15;
    }();
    return bits;
}

} // namespace VideoCommon
