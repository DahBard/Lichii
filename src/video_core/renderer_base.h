// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/settings_enums.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"

namespace Core::Frontend {
class EmuWindow;
class GraphicsContext;
} // namespace Core::Frontend

namespace VideoCore {

struct RendererDeviceInfo {
    std::string model;
    std::string driver;
    std::string api;
};

enum class FrameGenState : u8 {
    Pending,
    Running,
    UnsupportedRenderer,
    UnsupportedBuild,
    UnsupportedGpu,
    UnsupportedSurface,
    MissingLosslessDll,
};

struct FrameGenStatus {
    FrameGenState state{FrameGenState::UnsupportedRenderer};
    u64 generated_frames{};
    std::string idle_tag;    ///< AMD FSR frame generation not generating: a short reason for the bar
    std::string idle_reason; ///< and the full one for its tooltip.
};

/// The game-specific upscaler (Ocarina of Time's FSR / DLSS replacement), for the status bar.
struct UpscalerStatus {
    bool supported{};      ///< This build and platform have it at all
    u64 upscaled_frames{}; ///< Frames it upscaled so far (rises while it runs)
    std::string name;      ///< "FSR 4.1.2", "FSR 3.1.6", "DLSS"
    u32 render_width{}, render_height{}, output_width{}, output_height{};
    std::string idle_tag;    ///< Not upscaling: a short reason for the bar
    std::string idle_reason; ///< and the full one for its tooltip.
};

struct DlssStatus {
    u64 evaluations{};
    /// The last evaluation's settings: the mode it ran in, and the mode and preset the user chose
    Settings::NvidiaDlssQuality mode{};
    Settings::NvidiaDlssQuality requested{};
    Settings::NvidiaDlssPreset preset{};
};

struct RendererSettings {
    // Screenshot
    std::atomic<bool> screenshot_requested{false};
    void* screenshot_bits{};
    std::function<void(bool)> screenshot_complete_callback;
    Layout::FramebufferLayout screenshot_framebuffer_layout;
    Service::Nvnflinger::LayerStackId screenshot_layer_stack{Service::Nvnflinger::LayerStackId::Default};
};

class RendererBase {
public:
    YUZU_NON_COPYABLE(RendererBase);
    YUZU_NON_MOVEABLE(RendererBase);

    explicit RendererBase(Core::Frontend::EmuWindow& window,
                          std::unique_ptr<Core::Frontend::GraphicsContext> context);
    virtual ~RendererBase();

    /// Finalize rendering the guest frame and draw into the presentation texture
    virtual void Composite(std::span<const Tegra::FramebufferConfig> layers) = 0;

    /// Get the tiled applet layer capture buffer
    virtual std::vector<u8> GetAppletCaptureBuffer() = 0;

    [[nodiscard]] virtual RasterizerInterface* ReadRasterizer() = 0;

    [[nodiscard]] virtual std::string GetDeviceVendor() const = 0;

    /// Whether frame generation can run and how many frames it has generated so far
    [[nodiscard]] virtual FrameGenStatus GetFrameGenStatus() const {
        return {};
    }

    /// Whether the game-specific upscaler runs, which one and at what sizes
    [[nodiscard]] virtual UpscalerStatus GetUpscalerStatus() const {
        return {};
    }

    /// How many NVIDIA DLSS evaluations ran so far and the mode of the last one
    [[nodiscard]] virtual DlssStatus GetDlssStatus() const {
        return {};
    }

    /// Asks for one host GPU time sample of an upcoming frame
    virtual void RequestGpuBusySample() {}

    /// Host GPU busy time of the last sampled frame, nullopt when it isn't measured
    [[nodiscard]] virtual std::optional<double> GetGpuBusyMs() const {
        return std::nullopt;
    }

    [[nodiscard]] const RendererDeviceInfo& GetDeviceInfo() const {
        return device_info;
    }

    // Getter/setter functions:
    // ------------------------

    [[nodiscard]] f32 GetCurrentFPS() const {
        return m_current_fps;
    }

    [[nodiscard]] int GetCurrentFrame() const {
        return m_current_frame;
    }

    [[nodiscard]] Core::Frontend::GraphicsContext& Context() {
        return *context;
    }

    [[nodiscard]] const Core::Frontend::GraphicsContext& Context() const {
        return *context;
    }

    [[nodiscard]] Core::Frontend::EmuWindow& GetRenderWindow() {
        return render_window;
    }

    [[nodiscard]] const Core::Frontend::EmuWindow& GetRenderWindow() const {
        return render_window;
    }

    [[nodiscard]] RendererSettings& Settings() {
        return renderer_settings;
    }

    [[nodiscard]] const RendererSettings& Settings() const {
        return renderer_settings;
    }

    /// Refreshes the settings common to all renderers
    void RefreshBaseSettings();

    /// Returns true if a screenshot is being processed
    bool IsScreenshotPending() const;

    /// Request a screenshot of the next frame.
    void RequestScreenshot(void* data, std::function<void(bool)> callback,
                           const Layout::FramebufferLayout& layout,
                           Service::Nvnflinger::LayerStackId layer_stack =
                               Service::Nvnflinger::LayerStackId::Default);

protected:
    Core::Frontend::EmuWindow& render_window; ///< Reference to the render window handle.
    std::unique_ptr<Core::Frontend::GraphicsContext> context;
    f32 m_current_fps = 0.0f; ///< Current framerate, should be set by the renderer
    int m_current_frame = 0;  ///< Current frame, should be set by the renderer

    RendererSettings renderer_settings;
    RendererDeviceInfo device_info;

private:
    /// Updates the framebuffer layout of the contained render window handle.
    void UpdateCurrentFramebufferLayout();
};

} // namespace VideoCore
