#pragma once

#include <cstdint>

namespace FSRD::CyberpunkFogDepthCopy
{
// Only the authenticated Fog pixel-t0 R32_FLOAT view is covered. These are
// descriptor checks, not evidence of the current view, source lifetime or state.
inline constexpr uint32_t R32Typeless = 39, R32Float = 41;
inline constexpr uint32_t Texture2D = 3, UnknownLayout = 0;
inline constexpr uint32_t AllowRenderTarget = 1, AllowDepthStencil = 2, AllowUav = 4;
inline constexpr uint32_t MaxDimension = 8192; // Conservative one-shot diagnostic bound.

enum class SourceKind : uint8_t
{
    Refused,
    TypedR32Float,
    TypelessR32Depth
};

namespace Detail
{
// Structural template accepts D3D12_RESOURCE_DESC or an identical portable test
// description without including Windows headers or reading any resource object.
template<class Desc>
constexpr bool WholeExtent(const Desc& desc, uint32_t width, uint32_t height) noexcept
{
    return width && height && width <= MaxDimension && height <= MaxDimension &&
        uint32_t(desc.Dimension) == Texture2D && desc.Width == uint64_t(width) &&
        desc.Height == height && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
        uint32_t(desc.Layout) == UnknownLayout;
}
} // namespace Detail

template<class Desc>
constexpr SourceKind ClassifySource(const Desc& source, uint32_t currentSrvFormat,
                                    uint32_t width, uint32_t height) noexcept
{
    if (currentSrvFormat != R32Float || !Detail::WholeExtent(source, width, height))
        return SourceKind::Refused;
    const auto format = uint32_t(source.Format), flags = uint32_t(source.Flags);
    if (format == R32Float && (flags & ~(AllowRenderTarget | AllowUav)) == 0)
        return SourceKind::TypedR32Float;
    // Exact live source: R32_TYPELESS + ALLOW_DEPTH_STENCIL, ordinary R32_FLOAT
    // SRV. Do not generalize to D24S8/D32S8, other typeless formats, typed D32,
    // DENY_SHADER_RESOURCE or simultaneous-access resources.
    if (format == R32Typeless && flags == AllowDepthStencil)
        return SourceKind::TypelessR32Depth;
    return SourceKind::Refused;
}

template<class Desc>
constexpr bool AdmitDestination(const Desc& destination, uint32_t width, uint32_t height) noexcept
{
    return Detail::WholeExtent(destination, width, height) &&
        uint32_t(destination.Format) == R32Float && uint32_t(destination.Flags) == 0;
}

// CopyTextureRegion preserves data between members of one typeless format group.
// A depth-stencil source additionally requires a whole subresource, zero destination
// offsets and a null source box, with equal sample counts. Both admitted textures
// have exactly one mip/slice/plane and equal whole extents; use subresource0 on
// each, (0,0,0), nullptr. No shader, resolve, float conversion or depth linearization.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion
//
// Host obligations remain: authenticated current R32_FLOAT t0 view of mip0/plane0,
// distinct retained native/private resources on the same device, valid original-use
// lifetime, source COPY_SOURCE via the engine's RequestState+Flush (0x8c0 or a
// compatible read superset), destination COPY_DEST, and restoration of original
// bindings on every path. Transition only the owned destination to0xc0 afterward.
// Do not guess a native StateBefore or issue a raw undo barrier: the engine owns
// source state/first-use reconciliation. These predicates record no commands.
} // namespace FSRD::CyberpunkFogDepthCopy
