#pragma once
#include <cstdint>
#include <limits>

namespace FSRDSubmission
{
// D3D12 reports UINT64_MAX when a device is removed. It is not successful completion.
constexpr bool CanRecycle(bool submitted, bool signalFailed, uint64_t completed, uint64_t target)
{
    return submitted && !signalFailed && target != 0 && completed != std::numeric_limits<uint64_t>::max() &&
           completed >= target;
}
} // namespace FSRDSubmission
