# DLSS RR to FSR Ray Regeneration 1.1

This remains a Cyberpunk-oriented adapter, not a lossless translation of every NGX RR integration.

- `DenoiserMode=auto` now selects fused lighting (`1`). The legacy albedo-weighted split (`0`) remains an explicit experimental option. Albedo alone cannot recover genuine diffuse/specular radiance from combined noisy color.
- Both modes retain available specular hit distance. Missing optional hit distance reads as zero; missing required depth, normals, albedo or separately declared roughness fails validation. Packed normal-alpha roughness does not require a separate texture.
- Depth and roughness layout are per feature and explicitly copied when changing denoiser modes/providers. Simultaneous features cannot overwrite one another's interpretation.
- Albedos use the SDK sample's RGB10A2 format, with matching quantization before demodulation. This avoids rounding small albedo channels to zero in the old eight-bit buffers, without increasing bytes per pixel.
- The jitter conversion is intentionally `(+2*x/width, -2*y/height)`, matching AMD's SDK 2.2 sample camera and denoiser dispatch, despite the old header's pixel-space comment.
- Projection handedness is obtained from `clip.w / view.z`, independently of reversed depth. Signed view positions are used for world reconstruction; AMD receives absolute linear depth. Cyberpunk's positive-Z convention is preserved.
- Render dimensions are read before conversion. The feature reserves display-sized RR resources once and resets temporal/camera history on render-size changes. This uses more memory than exact-size allocation, but does not destroy in-flight resources on quality changes. Inputs larger than the creation capacity are rejected; a new display size needs a newly created feature. This is conservative history reset, not seamless DRS.
- Denoiser outputs have explicit UAV state declarations. They return to readable state even on failed dispatch. Debug bypasses do not change their states. Both modes allocate the two floor-filter scratch buffers.
- Skipped pixels initialize motion explicitly: the same texture is reused for composited color later, so unwritten pixels must not retain last frame's RGB values as motion. Invalid camera input also invalidates temporal history before resuming.

## Remaining limitations

World-space normals and origin-zero render-resolution resources are expected. The depth-motion channel is reconstructed from camera motion only: 2D NGX motion does not supply independent object motion along Z. The raster-light floor isolation and split-light mode remain heuristics. Other games, moving-object reprojection and final visual quality require runtime validation; these changes do not by themselves establish the cause of earlier GPU faults.

## Build and regression checks

MSBuild compiles all four RR HLSL kernels with the repository's DXC into the configuration's intermediate directory. C++ uses only those generated headers, so editing HLSL cannot silently retain stale bytecode. GitHub Actions builds (or local builds with `/p:RunFSRDTests=true`) also run the projection/render-size tests and eleven source-level integration guards. These checks do not replace live GPU validation. No workflow changes or extra GitHub token scopes are required.

Reference inputs: AMD FidelityFX SDK **2.2** `Samples/Denoisers/FidelityFX_Denoiser/dx12/{fsrapirendermodule,denoiserrendermodule}.cpp` and `Kits/FidelityFX/denoisers/include/ffx_denoiser.h`; NVIDIA NGX's bundled `external/nvngx_dlss_sdk/{nvsdk_ngx_defs_dlssd,nvsdk_ngx_defs}.h`. RR 1.2's four-signal contract is not substituted for the 1.1 interface.
