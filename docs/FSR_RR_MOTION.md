# Cyberpunk depth-motion conversion

The generic NVIDIA RR interface documents **2D motion**, while the bundled AMD SDK 2.2 / RR 1.1 requires XY previous-UV minus current-UV and Z `abs(previousLinearDepth)-abs(currentLinearDepth)`. A camera-only reconstruction cannot recover independent object movement along Z. NVIDIA's [RR integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md) does not define extra motion channels for arbitrary games.

## Producer verified, not fitted

Cyberpunk 2.31 supplies RGBA16F motion. Its installed shader caches were inspected offline with Microsoft's [DirectX Shader Compiler v1.9.2607](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2607). The actual shaders establish:

| Writer | XY | Z | W |
|---|---|---|---|
| `m_velocityBufferInit3D` camera initializer | Previous-minus-current NDC × `(0.5,-0.5)` | `(previousClip.z/max(previousClip.w,0.05)-currentHardwareDepth)*1000` | 0 |
| `renderstage_velocitybuffer` geometry shaders | Previous-minus-current NDC × `(0.5,-0.5)` | `(previousClip.z/previousClip.w-currentClip.z/currentClip.w)*1000` | 1 |

Geometry writers were verified for vehicle and skinned-mesh variants, including discarded eye/signage passes. Their vertex shaders provide current and previous geometry positions. W distinguishes these inspected writer classes; W=1 does not itself mean that an object moved, nor is it an invalid-motion marker.

Local provenance:

- `engine/staticshader_final.cache` SHA256: `bff160947aba8df144200247adc39b44c26322360628d875f7c7218ad26c59ff`.
- Camera shader offset `0xd1c336`, 2614 bytes, SHA256 `ff53b6b854f126a3807bc8ba78054ea67621a30d1d4d58d3b92408a3df2b4f1c`.
- `engine/shader_final.cache` SHA256: `339145371a3b5aaa08eb4ef82d558f445b632e28603ee0f3b4860270dfc3ccfa`.
- Vehicle/skinned velocity pixel shader offset `0x1c7bd7c`, 1960 bytes, SHA256 `b07f59d149ba0f25aeba50aa3f708f38dfb3d73c25fac64bca98db03d043bf85`.
- Discarded eye velocity pixel shader offset `0x9144faa`, 2920 bytes, SHA256 `74c03316a0817f1c320cdebf6575213a230c9dfeba7950ae56fbb36278456dba`.

The cache index names and GUIDs identify these shaders; DXIL shader-model declarations verify the actual stages. The historical [WolvenKit cache reader](https://github.com/WolvenKit/WolvenKit/blob/main/WolvenKit.RED4/Archive/IO/ShaderCacheReader.cs) provides the material-record schema, but its old PS/VS field labels are reversed for this cache. The extracted blobs and complete proprietary shader disassemblies are **not** committed. Local extraction scripts, disassemblies and metadata are under `/tmp/optiscaler-motion-producer.OuvgNo/`; reproduce disassembly with `dxc -dumpbin extracted.dxil`.

## Implemented mapping and limits

The extra-channel path is restricted to `Cyberpunk2077.exe`, hardware depth, RGBA16F motion, a conventional perspective depth projection and valid previous-frame history. It is not inferred from a four-channel format in other games.

For the conventional column-vector projection `clip.z=A*z+B`, `clip.w=W*z`:

```text
previousHardwareDepth = currentHardwareDepth + motion.z / 1000
previousViewZ = previousB / (previousW * previousHardwareDepth - previousA)
outputMotion.z = abs(previousViewZ) - currentAbsoluteLinearDepth
```

Historical hardware depth is decoded with the **previous projection**, including its near/far coefficients. Using the current projection would produce false motion when projection depth mapping changes. Off-center XY projections are harmless to this scalar depth formula; oblique or depth-offset projection shapes are rejected.

XY texture values are unchanged. NGX X/Y motion scales independently default to one pixel multiplier and are divided by the corresponding render dimension for AMD. They never multiply Z, whose AMD scale remains one. Jitter uses the pinned sample's NDC conversion. Nonfinite scalar inputs are rejected before conversion dispatch; frame time must be positive and finite, with an independent measured-clock fallback for missing/invalid time inputs.

First frame, explicit reset, resolution change, actual denoiser bypass and unsuccessful evaluation invalidate motion history. Camera position, view and projection history are published together only after successful denoising, composition and either upscaling or a final debug-output blit. Output-only visualization modes still run RR and keep valid history. Reset writes zero depth delta. Unsupported games/layouts retain documented camera-only depth motion. Individual malformed samples (unknown W, nonfinite data, historical hardware depth outside `[0,1]`, zero denominator or nonpositive/invalid historical depth) retain that camera-only value. For W=0, historical camera clip W at or below 0.05 also declines engine-Z decoding: the producer clamps this denominator, so it is not invertible there. W=1 geometry writers do not use that clamp. Derived motion is bounded to the existing FP16 output range.

The decoder is a shared C++/HLSL implementation in [FSRDDepthMotion.h](../OptiScaler/upscalers/ffx/FSRDDepthMotion.h), exercised directly by [the C++ tests](../tests/fsrd_input_math.cpp): handedness, both depth directions, finite/infinite projections, changed historical projection, independent object movement, reset-adjacent zero motion and malformed values. These tests do not substitute for GPU runtime regression tests.

## Capture corroboration

The six original captured frames independently agree with the extracted encoding. A refined comparison reprojects with XY plus `(previousJitter-currentJitter)` **in pixels**, bilinearly samples hardware depth and then linearizes it. It checks bounds, normal agreement, diffuse-albedo agreement and locally smooth depth; it does not reject large genuine temporal depth changes.

On the reference pair's W=1 subset, 82,615 qualifying pixels had median historical-depth disagreement of approximately `0.00001149` for decoded Z versus `0.00067053` for camera-only depth. Current indoor/outdoor W=0 median decoded-Z errors were approximately `0.00000905` and `0.00004602`. Disocclusions, shading-normal differences, FP16 input motion and capture stalls remain limitations. These measurements support the mapping, not a claim of measured temporal image-quality improvement or universal compatibility with future game patches.

The same offline inspection confirmed that Cyberpunk deliberately produces white/white material guides with normal `(-1/sqrt(3),-1/sqrt(3),-1/sqrt(3))` and roughness zero. Every captured white/white pixel matched that FP16-rounded pattern. Rewriting its specular guide to enforce an invented diffuse-plus-specular energy bound was therefore not a faithful translation.
