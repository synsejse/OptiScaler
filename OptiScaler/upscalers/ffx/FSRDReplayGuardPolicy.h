#pragma once
#include "FSRDCyberpunkPrivateResetPolicy.h"
#include <array>
#include <span>

namespace FSRD::ReplayGuardPolicy
{
using Recording = CyberpunkPrivateResetPolicy::Recording;

// Completed GPU work can release its resources while these scalar guards still
// reject replay of the old native recording. Host retains each list's COM identity
// and publishes guards before removing the original per-frame submission watch.
template<size_t Capacity> class Guards
{
    std::array<Recording, Capacity> _records {};
  public:
    size_t Find(uintptr_t list) const noexcept
    {
        for (size_t i = 0; i < Capacity; ++i) if (_records[i].list == list) return i;
        return Capacity;
    }
    Recording At(size_t index) const noexcept { return index < Capacity ? _records[index] : Recording {}; }
    bool Remember(std::span<const Recording> records) noexcept
    {
        auto next = _records; // All-or-nothing, including capacity failures.
        for (const auto record : records)
        {
            if (!record.Valid()) return false;
            size_t slot = Capacity;
            for (size_t i = 0; i < Capacity; ++i)
                if (next[i].list == record.list) { slot = i; break; }
            if (slot == Capacity)
                for (size_t i = 0; i < Capacity; ++i)
                    if (!next[i].list) { slot = i; break; }
            if (slot == Capacity) return false;
            if (record.generation > next[slot].generation) next[slot] = record;
        }
        _records = next;
        return true;
    }
    bool Allows(std::span<const Recording> current) const noexcept
    {
        for (const auto observed : current)
        {
            const auto slot = Find(observed.list);
            if (observed.list && slot != Capacity &&
                (!observed.Valid() || observed.generation <= _records[slot].generation)) return false;
        }
        return true;
    }
    bool Retire(Recording current) noexcept
    {
        const auto slot = Find(current.list);
        if (!current.Valid() || slot == Capacity || current.generation <= _records[slot].generation) return false;
        _records[slot] = {};
        return true;
    }
    bool Empty() const noexcept
    {
        for (const auto record : _records) if (record.list) return false;
        return true;
    }
};
}
