# AMD FidelityFX SDK 2.4.0 (FSR 4 and frame generation on Vulkan)

Headers from https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (tag v2.4.0), unmodified:

- `api/include/` and `upscalers/include/`: the FidelityFX API the FSR upscaler
  (`src/video_core/renderer_vulkan/vk_fsr4_upscaler.cpp`) is built against.
- `framegeneration/include/`: AMD's frame generation and its swapchain
  (`src/video_core/renderer_vulkan/vk_amd_frame_gen.cpp`).

AMD's signed Vulkan DLLs (from `Kits/FidelityFX/signedbin`) are not in this repository. On
Windows they are loaded at runtime from the folder of `lichii.exe`
(`src/video_core/renderer_vulkan/vk_ffx_runtime.cpp`). The lichii-tuned package workflow
downloads them from the SDK's v2.4.0 release and checks their SHA-256:

- `amd_fidelityfx_loader_vk.dll`: `38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9`
- `amd_fidelityfx_upscaler_vk.dll` (FSR 4, and AMD's FSR 3.1):
  `4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7`
- `amd_fidelityfx_framegeneration_vk.dll` (needed for AMD frame generation):
  `d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4`

A local build copies them next to `lichii.exe` when they are placed in `bin/` here. Without
them, Ocarina of Time keeps its own upscaler and AMD frame generation is unavailable.

The DLLs are redistributed under AMD's license in `LICENSE.md`, which ships with them as
`AMD-FidelityFX-LICENSE.md`.
