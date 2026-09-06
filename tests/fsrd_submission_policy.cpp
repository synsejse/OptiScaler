#include "../OptiScaler/resource_tracking/FSRDSubmissionPolicy.h"
#include <cassert>
#include <iostream>
#include <vector>

using FSRDSubmission::CanRecycle;

int main()
{
    constexpr auto removed = std::numeric_limits<uint64_t>::max();
    static_assert(!CanRecycle(false, false, 100, 1)); // Recorded, not submitted: never wait/recycle.
    static_assert(!CanRecycle(true, true, 100, 1));   // Signal failure is not completion.
    static_assert(!CanRecycle(true, false, removed, 1));
    static_assert(!CanRecycle(true, false, 100, 0));
    static_assert(!CanRecycle(true, false, 6, 7));
    static_assert(CanRecycle(true, false, 7, 7));
    static_assert(CanRecycle(true, false, 8, 7));

    struct Slot { bool submitted; uint64_t target; int constants; };
    std::vector<Slot> slots;
    // Simulate a reload queuing eight dispatches before any GPU completion. The former
    // modulo-three policy overwrote the first five; the production completion predicate must not.
    for (int frame = 0; frame < 8; ++frame)
    {
        for (const auto& slot : slots)
            assert(!CanRecycle(slot.submitted, false, 0, slot.target));
        slots.push_back({ false, 0, frame });
    }
    for (int i = 0; i < 8; ++i)
    {
        assert(slots[i].constants == i);
        slots[i].submitted = true;
        slots[i].target = i + 1;
        assert(CanRecycle(true, false, 3, slots[i].target) == (i < 3));
    }
    // Independently numbered queues cannot share a completion counter.
    assert(CanRecycle(true, false, 10, 10));
    assert(!CanRecycle(true, false, 2, 10));
    std::cout << "FSRD submission recycling tests passed\n";
}
