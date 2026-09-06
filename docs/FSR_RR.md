# DLSS RR to FSR Ray Regeneration 1.1

This remains a Cyberpunk-oriented adapter, not a lossless translation of every NGX RR integration.

- Only AMD's fused single-signal mode is supported. The albedo-weighted diffuse/specular split has been removed from the converter, denoiser dispatch, composition, GUI and configuration. Old `DenoiserMode` entries are ignored and deleted when saving settings; they cannot restore the guessed split.
- Available specular hit distance is retained. Missing optional hit distance reads as zero; missing required depth, normals, albedo or separately declared roughness fails validation. Packed normal-alpha roughness does not require a separate texture.
- Depth and roughness layout are per feature and explicitly copied when changing providers/recreating the backend. Simultaneous features cannot overwrite one another's interpretation.
- Albedos use the SDK sample's RGB10A2 format, with matching quantization before demodulation. This avoids rounding small albedo channels to zero in the old eight-bit buffers, without increasing bytes per pixel.
- The jitter conversion is intentionally `(+2*x/width, -2*y/height)`, matching AMD's SDK 2.2 sample camera and denoiser dispatch, despite the old header's pixel-space comment.
- Projection handedness is obtained from `clip.w / view.z`, independently of reversed depth. Signed view positions are used for world reconstruction; AMD receives absolute linear depth. Cyberpunk's positive-Z convention is preserved.
- Render dimensions are read before conversion. The feature reserves display-sized RR resources once and resets temporal/camera history on render-size changes. This uses more memory than exact-size allocation, but does not destroy in-flight resources on quality changes. Inputs larger than the creation capacity are rejected; a new display size needs a newly created feature. This is conservative history reset, not seamless DRS.
- The denoiser output has an explicit UAV state declaration. It returns to readable state even on failed dispatch. Debug bypasses do not change its state. Two floor-filter scratch buffers are still needed; removing the second would break the filter even though only one lighting signal is denoised. Converted textures use normal owning objects rather than an aliased union of smart pointers.
- Skipped pixels initialize motion explicitly: the same texture is reused for composited color later, so unwritten pixels must not retain last frame's RGB values as motion. Invalid camera input also invalidates temporal history before resuming.

## What fused lighting means

The game supplies a combined noisy colour, not independently rendered diffuse and specular lighting. AMD's native single-signal mode accepts this combined lighting, normalized by a shared material colour:

`fusedAlbedo = max(diffuseAlbedo, specularAlbedo)` (component-wise, with a small positive floor)

`denoiserInput.rgb = combinedLighting / fusedAlbedo`

After denoising, composition multiplies the result by that same albedo. This demodulation/remodulation prevents the denoiser from confusing material colour with lighting variation; it is not spatial downsampling or a synthetic lighting split. Reflection hit distance accompanies radiance in alpha. Separate diffuse/specular albedos, normals, roughness, depth and motion remain available as guides. Internal FP16 radiance and RGB10A2 albedo storage follow the existing integration; this change does not reduce precision or sampling quality.

Genuinely separate signals can retain information that was lost when the renderer combined them. That would require real diffuse/specular lighting from the engine, not an albedo-based estimate. For example, the same material can be mostly diffuse-lit in one situation and mostly reflecting a bright light in another. The removed split could not distinguish those cases; algebraically its two demodulated outputs collapsed to the same lighting value. We do not trade input correctness for an assumed performance or quality benefit.

## Remaining limitations

World-space normals and origin-zero render-resolution resources are expected. The depth-motion channel is reconstructed from camera motion only: 2D NGX motion does not supply independent object motion along Z. Raster-light floor isolation and the raw/denoised correlation blend remain heuristics: the bridge estimates a component to preserve outside denoising and adds it back afterward. These are not diffuse/specular lighting separation, but they mean the complete adapter is not lossless or equivalent to native engine integration. Their behaviour and defaults were not changed by removing split mode; quality-first replacements need controlled visual comparisons, not assumed improvements. Other games, moving-object reprojection and final visual quality require runtime validation; these changes do not by themselves establish the cause of earlier GPU faults.

## Build and regression checks

MSBuild compiles all four RR HLSL kernels with the repository's DXC into the configuration's intermediate directory. C++ uses only those generated headers, so editing HLSL cannot silently retain stale bytecode. GitHub Actions builds (or local builds with `/p:RunFSRDTests=true`) also run the projection/render-size tests and fifteen source-level integration guards, including fused-only dispatch, legacy-config migration and C++/HLSL resource-slot agreement. These checks do not replace live GPU validation. No workflow changes or extra GitHub token scopes are required.

Reference inputs: AMD FidelityFX SDK **2.2** `Samples/Denoisers/FidelityFX_Denoiser/dx12/{fsrapirendermodule,denoiserrendermodule}.cpp` and `Kits/FidelityFX/denoisers/include/ffx_denoiser.h`; NVIDIA NGX's bundled `external/nvngx_dlss_sdk/{nvsdk_ngx_defs_dlssd,nvsdk_ngx_defs}.h`. RR 1.2's four-signal contract is not substituted for the 1.1 interface.
