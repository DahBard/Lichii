# AMD FSR API headers

`include/` holds the AMD FSR API headers that Lichii's FSR 4 path compiles against, from the
[AMD FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v2.4.0 (commit
`7539ae8`, MIT, see each file's header): `Kits/FidelityFX/api/include/ffx_api.h`,
`ffx_api_types.h`, `vk/ffx_api_vk.h` and `Kits/FidelityFX/upscalers/include/ffx_upscale.h`.

Changes: `ffx_upscale.h` includes its neighbours by name, and `ffx_api.h` declares its entry
points without `__declspec(dllexport)` outside Windows.

Nothing of AMD's binaries is in this repository. The signed `amd_fidelityfx_loader_vk.dll` and
`amd_fidelityfx_upscaler_vk.dll` are loaded at run time from the executable's folder; the
lichii-tuned build copies them there from the SDK's `Kits/FidelityFX/signedbin/`, with the SDK's
license (`Kits/FidelityFX/docs/license.md`, binary redistribution only).
