#pragma once

#include <sl_core_types.h>
#include <cstdint>

// Diagnostic provenance only. Never selects a rendering mode or supplies camera/history.
// A scope observes the public SL call's token, not a Fog frame or GPU execution order.
namespace FSRD::SlEvaluationProvenance
{
enum class ViewportStatus : uint8_t
{
    Missing,
    Observed,
    Ambiguous,
    InvalidInput,
    UnsupportedVersion,
    InputLimit,
    ChainLimit
};

inline const char* ViewportStatusName(ViewportStatus status) noexcept
{
    switch (status)
    {
    case ViewportStatus::Observed: return "observed";
    case ViewportStatus::Ambiguous: return "ambiguous";
    case ViewportStatus::InvalidInput: return "invalid_input";
    case ViewportStatus::UnsupportedVersion: return "unsupported_version";
    case ViewportStatus::InputLimit: return "input_limit";
    case ViewportStatus::ChainLimit: return "chain_limit_or_cycle";
    default: return "missing";
    }
}

struct Snapshot
{
    bool observed = false;
    uint32_t frameIndex = 0;
    uint32_t feature = 0;
    uintptr_t commandBuffer = 0; // Original SL argument address, NOT a canonical native-list identity.
    ViewportStatus viewportStatus = ViewportStatus::Missing;
    uint32_t viewport = 0; // Valid only when viewportStatus == Observed; zero is not a fallback.
    uint32_t depth = 0;
};

inline constexpr uint32_t MaxInputs = 32;
inline constexpr uint32_t MaxInputNodes = 64;
inline constexpr uint32_t MaxScopeDepth = 32;

namespace Detail
{
inline thread_local Snapshot current {};

inline void ReadViewport(Snapshot& result, const sl::BaseStructure* const* inputs, uint32_t count) noexcept
{
    if (count > MaxInputs)
    {
        result.viewportStatus = ViewportStatus::InputLimit;
        return;
    }
    if (count && !inputs)
    {
        result.viewportStatus = ViewportStatus::InvalidInput;
        return;
    }

    // All pointers are borrowed only during the actual public API call. Inspect only the
    // public header, then the public ViewportHandle type after exact GUID/version checks.
    // Bound every chain and reject repeats; no opaque struct payload is dereferenced.
    const sl::BaseStructure* visited[MaxInputNodes] {};
    uint32_t visitedCount = 0;
    bool foundViewport = false;
    for (uint32_t i = 0; i < count; ++i)
    {
        const auto* node = inputs[i];
        if (!node)
        {
            result.viewportStatus = ViewportStatus::InvalidInput;
            return;
        }
        while (node)
        {
            if (visitedCount == MaxInputNodes)
            {
                result.viewportStatus = ViewportStatus::ChainLimit;
                return;
            }
            for (uint32_t j = 0; j < visitedCount; ++j)
                if (visited[j] == node)
                {
                    result.viewportStatus = ViewportStatus::ChainLimit;
                    return;
                }
            visited[visitedCount++] = node;
            if (node->structType == sl::ViewportHandle::s_structType)
            {
                if (node->structVersion != sl::kStructVersion1)
                {
                    result.viewportStatus = ViewportStatus::UnsupportedVersion;
                    return;
                }
                if (foundViewport)
                {
                    result.viewportStatus = ViewportStatus::Ambiguous;
                    return;
                }
                result.viewport = static_cast<uint32_t>(*static_cast<const sl::ViewportHandle*>(node));
                foundViewport = true;
            }
            node = node->next;
        }
    }
    result.viewportStatus = foundViewport ? ViewportStatus::Observed : ViewportStatus::Missing;
}
} // namespace Detail

// Return only a by-value scalar snapshot; no SL/frame/input pointer escapes the scope.
inline Snapshot Current() noexcept { return Detail::current; }

class Scope
{
  public:
    Scope(bool enabled, sl::Feature feature, const sl::FrameToken& frame,
          const sl::BaseStructure* const* inputs, uint32_t numInputs, sl::CommandBuffer* commandBuffer) noexcept
        : _previous(Detail::current)
    {
        Snapshot next {};
        next.depth = _previous.depth >= MaxScopeDepth ? MaxScopeDepth : _previous.depth + 1;
        if (enabled && _previous.depth < MaxScopeDepth)
        {
            try
            {
                next.frameIndex = static_cast<uint32_t>(frame);
                next.feature = static_cast<uint32_t>(feature);
                next.commandBuffer = reinterpret_cast<uintptr_t>(commandBuffer);
                Detail::ReadViewport(next, inputs, numInputs);
                next.observed = true;
            }
            catch (...)
            {
                next = {}; // Metadata failure must never prevent the original SL call.
                next.depth = _previous.depth + 1;
            }
        }
        // Even an inactive/unavailable nested evaluation masks its outer token: a nested
        // NGX call must not be attributed to a different SL evaluation merely by proximity.
        Detail::current = next;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
    ~Scope() { Detail::current = _previous; }

  private:
    Snapshot _previous;
};
} // namespace FSRD::SlEvaluationProvenance
