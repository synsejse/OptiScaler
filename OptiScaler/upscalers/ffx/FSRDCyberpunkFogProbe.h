#pragma once

#include <d3d12.h>
#include <cstdint>

// Research-only probe for one authenticated Cyberpunk executable. Metadata-only
// unless the separate CyberpunkFogCapture INI opt-in AND an explicit one-shot
// FSRRR-fog-capture.request marker are present. That capture copies the original
// target and repeats the authenticated shader only against a private RGBA target;
// it does not implement a fog correction or change the normal RR input mapping.
// Initialize at device setup, never under DllMain's loader lock. Once installed,
// the hooks remain until process exit; changing the opt-in requires a restart.
// Creating FSRRR-fog-probe.request beside the game exe re-arms the first-32-draw
// log budget (at most twice/process); the marker is consumed, no game files changed.
namespace FSRDCyberpunkFogProbe
{
void Initialize(bool enabled);
void HookDevice(ID3D12Device* device);
void HookCommandList(ID3D12GraphicsCommandList* commandList);
// Call before RR input conversion with the actual NGX resource pointers. No-op
// except for eight endpoint observations after an explicitly requested fog capture.
// Reports resource identity and local recording order, NOT GPU/frame association
// or unchanged contents. It records no GPU commands and changes no NGX inputs.
void ObserveNgxInput(ID3D12GraphicsCommandList* commandList, ID3D12Resource* color,
                     ID3D12Resource* colorBeforeParticles, uint64_t featureId, uint64_t frameIndex,
                     UINT renderWidth, UINT renderHeight) noexcept;
} // namespace FSRDCyberpunkFogProbe
