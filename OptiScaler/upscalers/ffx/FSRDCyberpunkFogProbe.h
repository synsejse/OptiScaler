#pragma once

#include <d3d12.h>

// Research-only, metadata-only probe for one authenticated Cyberpunk executable.
// Initialize at device setup, never under DllMain's loader lock. Once installed,
// the hooks remain until process exit; changing the opt-in requires a restart.
// Creating FSRRR-fog-probe.request beside the game exe re-arms the first-32-draw
// log budget (at most twice/process); the marker is consumed, no game files changed.
namespace FSRDCyberpunkFogProbe
{
void Initialize(bool enabled);
void HookDevice(ID3D12Device* device);
void HookCommandList(ID3D12GraphicsCommandList* commandList);
} // namespace FSRDCyberpunkFogProbe
