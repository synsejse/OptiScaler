#pragma once

#include <cstdint>
#include <string>

namespace FSRDCyberpunkEarlyGuides
{
// CPU metadata only. Caller must authenticate the exact Cyberpunk image and
// accept an explicit one-shot fog capture request, with this FogNode context
// still live. Includes raw current-view camera fields/matrix rows, not bound GPU
// constants or a reconstructed camera. No engine functions, COM calls, GPU
// commands or retained pointers; independent reads are not an atomic snapshot.
// A nonzero handle is NOT proof of initialization, resource state or same frame.
std::string Describe(const void* context, uintptr_t authenticatedImageBase) noexcept;
}
