#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace wallpaper {
    struct ScreenSize { int w, h; };
    struct VideoImportResult {
        bool copied = false;
        std::string encoder;
        ScreenSize output{0, 0};
    };

    inline ScreenSize import_size(ScreenSize source, ScreenSize screen) {
        if (source.w < 2 || source.h < 2 || screen.w < 2 || screen.h < 2) {
            throw std::runtime_error("Invalid video or screen dimensions");
        }
        const double scale = (std::min)({1.0,
            static_cast<double>(screen.w) / source.w,
            static_cast<double>(screen.h) / source.h});
        // Preserve aspect ratio, never upscale, and align for YUV420 encoders.
        return {
            (std::max)(2, static_cast<int>(std::floor(source.w * scale)) & ~1),
            (std::max)(2, static_cast<int>(std::floor(source.h * scale)) & ~1)
        };
    }
}
