#pragma once
#include "FSRDSubmissionPolicy.h"
#include "ResTrack_dx12.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace FSRDSubmission
{
using Microsoft::WRL::ComPtr;

struct Ticket
{
    // One NGX evaluation recording is submitted once; replaying its old command list without
    // reevaluating is not supported (it would also replay provider temporal-history writes).
    ComPtr<ID3D12CommandList> list;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    std::vector<std::shared_ptr<void>> owners;
    uint64_t value = 0;
    bool submitted = false;
    bool signalFailed = false;
};

struct QueueProgress
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
};

struct Registry
{
    std::mutex mutex;
    std::vector<std::shared_ptr<Ticket>> pending;
    std::vector<QueueProgress> queues;
    std::atomic<bool> hasPending { false };
};

inline Registry& GetRegistry()
{
    // Unsubmitted work (including abandoned lists) and failed signals cannot safely be freed.
    // Bounded retention deliberately survives module/process teardown without a GPU wait.
    static auto* registry = new Registry;
    return *registry;
}

inline bool CompleteLocked(const Ticket& ticket)
{
    return ticket.fence && CanRecycle(ticket.submitted, ticket.signalFailed,
                                       ticket.fence->GetCompletedValue(), ticket.value);
}

inline bool Complete(const std::shared_ptr<Ticket>& ticket)
{
    if (!ticket)
        return true;
    auto& registry = GetRegistry();
    std::lock_guard lock(registry.mutex);
    return CompleteLocked(*ticket);
}

inline void CollectCompletedLocked(Registry& registry, std::vector<std::shared_ptr<Ticket>>& retired)
{
    for (auto it = registry.pending.begin(); it != registry.pending.end();)
    {
        if (CompleteLocked(**it))
        {
            retired.push_back(std::move(*it));
            it = registry.pending.erase(it);
        }
        else
            ++it;
    }
}

inline std::shared_ptr<Ticket> Retain(ID3D12Device* device, ID3D12CommandList* list,
                                      const std::shared_ptr<void>& owner)
{
    // Resolve Streamline's wrapper to the identity passed to the actual queue submission.
    auto* submittedList = ResTrack_Dx12::PrepareSubmission(device, list);
    if (!submittedList)
        throw std::runtime_error("FSRD submission observer unavailable");

    auto& registry = GetRegistry();
    // Destroy COM-backed owners only after unlocking, since final Release can enter other hooks.
    std::vector<std::shared_ptr<Ticket>> retired;
    std::lock_guard lock(registry.mutex);
    CollectCompletedLocked(registry, retired);

    for (const auto& ticket : registry.pending)
    {
        if (!ticket->submitted && !ticket->signalFailed && ticket->list.Get() == submittedList)
        {
            ticket->owners.push_back(owner);
            return ticket;
        }
    }

    if (registry.pending.size() >= 64)
        throw std::runtime_error("FSRD pending submission limit reached; refusing unsafe descriptor reuse");

    auto ticket = std::make_shared<Ticket>();
    ticket->list = submittedList;
    ticket->device = device;
    ticket->owners.push_back(owner);
    registry.pending.push_back(ticket);
    registry.hasPending.store(true, std::memory_order_release);
    return ticket;
}

using Submission = std::vector<std::shared_ptr<Ticket>>;

inline Submission Preparing(UINT count, ID3D12CommandList* const* lists)
{
    auto& registry = GetRegistry();
    if (!registry.hasPending.load(std::memory_order_acquire))
        return {};

    std::lock_guard lock(registry.mutex);
    Submission submission;
    for (const auto& ticket : registry.pending)
    {
        if (!ticket->submitted && !ticket->signalFailed &&
            std::find(lists, lists + count, ticket->list.Get()) != lists + count)
        {
            // Detach this recording before ExecuteCommandLists. A different thread can reset and
            // re-record the same list immediately afterward; that work needs its own ticket.
            ticket->submitted = true;
            submission.push_back(ticket);
        }
    }
    return submission;
}

inline void Submitted(ID3D12CommandQueue* queue, const Submission& submission)
{
    if (submission.empty())
        return;

    auto& registry = GetRegistry();
    Submission retired; // Declared before lock so COM releases occur after unlocking.
    std::lock_guard lock(registry.mutex);
    // Use separate monotonically increasing timelines for distinct queues. A signal on another
    // queue must never make storage used by unfinished work look reusable.
    auto progress = std::find_if(registry.queues.begin(), registry.queues.end(),
                                  [&](const auto& item) { return item.queue.Get() == queue; });
    HRESULT result = S_OK;
    if (progress == registry.queues.end())
    {
        if (registry.queues.size() >= 16)
            result = E_OUTOFMEMORY;
        else
        {
            QueueProgress created;
            created.queue = queue;
            result = submission.front()->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&created.fence));
            if (SUCCEEDED(result))
            {
                registry.queues.push_back(std::move(created));
                progress = std::prev(registry.queues.end());
            }
        }
    }
    if (SUCCEEDED(result))
        result = queue->Signal(progress->fence.Get(), ++progress->value);

    for (const auto& ticket : submission)
    {
        ticket->signalFailed = FAILED(result);
        if (SUCCEEDED(result))
        {
            ticket->fence = progress->fence;
            ticket->value = progress->value;
        }
    }
    if (FAILED(result))
        LOG_ERROR("FSRD submission fence failed: {:X}; retaining dispatch storage", static_cast<UINT>(result));

    CollectCompletedLocked(registry, retired);
    registry.hasPending.store(!registry.pending.empty(), std::memory_order_release);
}
} // namespace FSRDSubmission
