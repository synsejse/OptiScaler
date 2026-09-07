#pragma once

#include "FSRDCyberpunkPrivateResetPolicy.h"
#include <array>
#include <limits>
#include <optional>
#include <algorithm>
#include <vector>

namespace FSRD::CyberpunkTemporalWindowPolicy
{
namespace FramePolicy = CyberpunkPrivateResetPolicy;
using Recording = FramePolicy::Recording;
using Queue = FramePolicy::Queue;
using ProducerSeal = FramePolicy::ProducerSeal;

enum class Role : uint8_t { Ray, Guides, Fog };
enum class Failure : uint8_t
{
    None, InvalidWindow, InvalidFrame, InvalidRole, RepeatedRole, OutsideLookAhead,
    ConsumerNotCurrent, WindowStopped, MissingRole, FramePolicyRefused, WrongQueue,
    ReceiptCapacity, InvalidReturnReceipt, ConsumerNotReturned, FrameFailed
};

struct FrameKey
{
    uint64_t epoch = 0;
    uint32_t index = 32, frame = 0;
    uint32_t frameCount = 32;
    constexpr bool Valid() const noexcept
    { return epoch && ((frameCount == 0 && index != UINT32_MAX) ||
                      ((frameCount == 32 || frameCount == 18000) && index < frameCount)); }
    // The visual pass has no automatic readbacks, including at its last frame.
    constexpr bool CaptureFinal() const noexcept { return Valid() && frameCount == 32 && index == 31; }
    bool operator==(const FrameKey&) const = default;
};

struct Receipt
{
    uint64_t epoch = 0, serial = 0;
    uintptr_t queue = 0;
    constexpr bool Valid() const noexcept { return epoch && serial && queue; }
    bool operator==(const Receipt&) const = default;
};

struct SubmissionDecision
{
    bool allowed = false;
    Failure failure = Failure::None;
    FramePolicy::Failure frameFailure = FramePolicy::Failure::None;
    Receipt receipt {}; // Empty on an observed unrelated/empty call; no return notification needed.
};

struct ReturnDecision
{
    bool allowed = false;
    Failure failure = Failure::None;
    FrameKey consumer {}; // Valid ONLY for a newly returned consumer, never a producer-only return.
    uint32_t producersReturned = 0;
};

// Value-only routing for one history session, bounded or continuous. Host serializes every
// method with its window lock, never held across native Execute/provider calls.
// The caller authenticates epoch, actual source frames, canonical same-device
// DIRECT queue, list/Reset generations, full intra-frame camera equality, actual
// original-call returns, resource ownership and successful scene restoration.
// This policy discovers none of those facts and contains no clock/native reads,
// COM ownership, GPU work, denoiser Session, resource pool or camera history.
// Epoch must uniquely identify this window within the host's receipt lifetime.
class Window
{
  public:
    static constexpr uint32_t FrameCount = 32;
    static constexpr uint32_t VisualFrameCount = 18000;
    static constexpr uint32_t ContinuousFrameCount = 0;
    static constexpr size_t MaxVisualWatches = 64;
    // Only current and one producer look-ahead frame can acquire roles. Four is
    // a conservative bound on outstanding related native calls, not a D3D limit.
    static constexpr size_t MaxPendingCalls = 4, MaxFramesPerCall = 2;

    Window(uint64_t epoch, Queue queue, uint32_t observedFirstFrame, uint32_t frameCount = FrameCount)
        : _epoch(epoch), _queue(queue), _first(observedFirstFrame), _frameCount(frameCount)
    {
        if (!epoch || !queue.queue || !queue.device || !queue.direct ||
            (frameCount != FrameCount && frameCount != VisualFrameCount && frameCount != ContinuousFrameCount) ||
            (frameCount && observedFirstFrame > std::numeric_limits<uint32_t>::max() - (frameCount - 1)))
        {
            StopWith(Failure::InvalidWindow);
            return;
        }
        // Continuous slots are reused only after both native Reset generations
        // advanced and every exact return receipt drained. A key also contains
        // its absolute ordinal, so an old key cannot bind a reused slot.
        _frames = std::vector<Frame>(frameCount ? frameCount : MaxVisualWatches);
        _watches.reserve(frameCount == FrameCount ? FrameCount : MaxVisualWatches);
        for (auto& frame : _frames) frame.policy.emplace(queue.device);
        _valid = true;
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // actualFrame is the host-observed current source, not an estimated index.
    // Returned key remains immutable even as another CPU callback advances the
    // current frame. New Fog consumers cannot enter the look-ahead slot.
    FrameKey ClaimRole(uint32_t actualFrame, Role role) noexcept
    {
        if (!_valid || actualFrame < _first || actualFrame - _first == UINT32_MAX ||
            (_frameCount && uint64_t(actualFrame) - _first >= _frameCount))
        { StopWith(Failure::InvalidFrame); return {}; }
        const uint32_t index = actualFrame - _first;
        const auto bit = RoleBit(role);
        if (!bit) { StopWith(Failure::InvalidRole); return {}; }
        auto& frame = At(index);
        if (frame.roles && frame.index != index)
        { StopWith(Failure::ReceiptCapacity); return {}; }
        if (frame.roles & bit) { StopWith(Failure::RepeatedRole); return {}; }
        if (index < _committed || index > _committed + 1)
        { StopWith(Failure::OutsideLookAhead); return {}; }
        if (role == Role::Fog && index != _committed)
        { StopWith(Failure::ConsumerNotCurrent); return {}; }
        // A soft stop forbids new frames/consumers but permits missing producer
        // roles of an already-started frame to close an existing obligation.
        // Never drop an embedded read merely because a later frame was refused.
        if (_stopped && (!frame.roles || role == Role::Fog)) return {};
        if (!frame.roles)
        {
            if (_frameCount != FrameCount && _watches.size() == MaxVisualWatches)
            { StopWith(Failure::ReceiptCapacity); return {}; }
            frame.index = index;
            if (!frame.policy) frame.policy.emplace(_queue.device);
            _watches.push_back(index); // Capacity reserved before any native recording.
        }
        frame.roles |= bit;
        return Key(index);
    }

    bool DeclareProducer(FrameKey key, Recording recording) noexcept
    {
        auto* frame = Find(key);
        if (!frame || !(frame->roles & RoleBit(Role::Ray))) return MissingRole();
        if (!frame->policy->DeclareProducer(recording)) return RefuseFrame(*frame);
        frame->producer = recording;
        return true;
    }
    bool SealProducer(FrameKey key, const ProducerSeal& seal) noexcept
    {
        auto* frame = Find(key);
        if (!frame || (frame->roles & ProducerRoles) != ProducerRoles) return MissingRole();
        if (!frame->policy->SealProducer(seal)) return RefuseFrame(*frame);
        return true;
    }
    bool EmbedConsumer(FrameKey key, Recording recording, uint64_t firstReadOrdinal, bool ownersRetained,
                       bool drainStopped = false) noexcept
    {
        auto* frame = Find(key);
        if (!frame || !(frame->roles & RoleBit(Role::Fog))) return MissingRole();
        // Only a consumer claimed before an expected stop may finish recording.
        // ClaimRole still forbids new Fog roles after Stop, and a hard failure
        // never gains this permission. Legacy bounded controls are unchanged.
        if (_stopped && !(Continuous() && drainStopped && _failure == Failure::WindowStopped)) return false;
        if (key.index != _committed) { StopWith(Failure::ConsumerNotCurrent); return false; }
        if (!frame->policy->EmbedConsumer(recording, firstReadOrdinal, ownersRetained)) return RefuseFrame(*frame);
        frame->consumer = recording;
        return true;
    }
    bool SealConsumer(FrameKey key, Recording recording, uint64_t terminalOrdinal, bool succeeded,
                      bool restored) noexcept
    {
        auto* frame = Find(key);
        if (!frame || !(frame->roles & RoleBit(Role::Fog))) return MissingRole();
        if (!frame->policy->SealConsumer(recording, terminalOrdinal, succeeded, restored)) return RefuseFrame(*frame);
        return true;
    }

    // Call before the exact native Execute AND before submission-fence bookkeeping.
    // Iterate all started policies, including completed tombstones, so a duplicate
    // old same-generation command list never disappears as "unrelated". Unknown
    // generations follow the existing per-frame policy, not a cached role guess.
    SubmissionDecision BeforeExecute(Queue actualQueue, std::span<const Recording> recordings) noexcept
    {
        if (!_valid) return RejectSubmission(Failure::InvalidWindow);
        if (recordings.empty()) return { true };
        if (recordings.size() > FramePolicy::Policy::MaxExecuteLists)
            return RejectSubmission(Failure::FramePolicyRefused, FramePolicy::Failure::ArrayTooLarge);
        Call call;
        for (const uint32_t index : _watches)
        {
            auto& frame = At(index);
            if (!frame.roles) continue;
            const auto decision = frame.policy->BeforeExecute(actualQueue, recordings);
            if (!decision.Allowed()) return RejectSubmission(Failure::FramePolicyRefused, decision.failure);
            if (!decision.token) continue;
            if (actualQueue != _queue) return RejectSubmission(Failure::WrongQueue);
            if (call.count == MaxFramesPerCall) return RejectSubmission(Failure::ReceiptCapacity);
            call.entries[call.count++] = { index, decision.token };
        }
        if (!call.count) return { true };
        for (auto& pending : _calls)
        {
            if (pending.receipt.Valid()) continue;
            // At most two related submissions per each bounded non-reusable frame
            // policies; serial cannot approach uint64 overflow in a valid window.
            call.receipt = { _epoch, ++_nextCall, _queue.queue };
            pending = call;
            return { true, Failure::None, FramePolicy::Failure::None, call.receipt };
        }
        return RejectSubmission(Failure::ReceiptCapacity);
    }

    // The caller invokes this ONLY after this exact original Execute returned.
    // Receipt validation binds local policy tokens to the recorded call, but is
    // not an independent observation of native return or GPU completion.
    ReturnDecision AfterExecute(Receipt receipt) noexcept
    {
        for (auto& pending : _calls)
        {
            if (!receipt.Valid() || pending.receipt != receipt) continue;
            ReturnDecision result { true };
            for (uint32_t i = 0; i < pending.count; ++i)
            {
                const auto entry = pending.entries[i];
                if (At(entry.index).index != entry.index || !At(entry.index).policy) return RejectReturn();
                auto& policy = *At(entry.index).policy;
                const bool producerBefore = policy.ProducerReturned(), consumerBefore = policy.ConsumerReturned();
                if (!policy.AfterExecute(entry.token)) return RejectReturn();
                result.producersReturned += !producerBefore && policy.ProducerReturned();
                if (!consumerBefore && policy.ConsumerReturned())
                {
                    if (result.consumer.Valid()) return RejectReturn();
                    result.consumer = Key(entry.index);
                }
            }
            pending = {};
            return result;
        }
        return RejectReturn();
    }

    // HOST COMMIT ATTESTATION: acknowledge the exact Work in PrivateDenoise::Session
    // and publish its OWN previous camera under the window lock before calling.
    // Producer return, scene recording alone and a fence guess cannot commit.
    // This policy deliberately does not own/call Session or manufacture camera data.
    bool CommitConsumer(FrameKey key, bool drainStopped = false) noexcept
    {
        auto* frame = Find(key);
        if (!frame || (_stopped && !(Continuous() && drainStopped))) return false;
        if (key.index != _committed || !frame->policy->ConsumerReturned())
        { StopWith(Failure::ConsumerNotReturned); return false; }
        ++_committed;
        return true;
    }

    void Stop() noexcept { StopWith(Failure::WindowStopped); }
    void FailFrame(FrameKey key) noexcept
    {
        if (auto* frame = Find(key)) frame->policy->Fail();
        StopWith(Failure::FrameFailed);
    }
    bool Stopped() const noexcept { return _stopped; }
    bool Complete() const noexcept { return _frameCount && !_stopped && _committed == _frameCount; } // Not GPU completion.
    bool Continuous() const noexcept { return _frameCount == ContinuousFrameCount; }
    size_t StorageFrames() const noexcept { return _frames.size(); }
    uint32_t CommittedFrames() const noexcept { return _committed; }
    Failure LastFailure() const noexcept { return _failure; }
    bool ConsumerEmbedded(FrameKey key) const noexcept
    { const auto* frame = FindConst(key); return frame && frame->policy->ConsumerEmbedded(); }
    bool ConsumerReturned(FrameKey key) const noexcept
    { const auto* frame = FindConst(key); return frame && frame->policy->ConsumerReturned(); }
    bool ProducerReturned(FrameKey key) const noexcept
    { const auto* frame = FindConst(key); return frame && frame->policy->ProducerReturned(); }
    // A soft window stop must not poison a previously embedded valid frame.
    // Invalid keys fail closed without changing any existing frame obligation.
    bool FrameFailed(FrameKey key) const noexcept
    { const auto* frame = FindConst(key); return !frame || frame->policy->Failed(); }

    // HOST ATTESTATION: both identities have a known, newer successful native
    // Reset. Old commands no longer exist on either list. This only stops
    // scanning an obsolete recording, never deletes a packet/policy or retires
    // GPU resources. The host separately requires its final fence and preserves
    // all CPU packet addresses for the bounded visual pass; continuous mode
    // separately requires every CPU reader to release before slot reuse. Unknown generations,
    // one-sided Reset and outstanding return receipts retain the watch.
    bool RetireSubmissionWatch(FrameKey key, Recording producer, Recording consumer) noexcept
    {
        const auto* frame = FindConst(key);
        if (_frameCount == FrameCount || !frame || key.index >= _committed || frame->policy->Failed() ||
            !frame->policy->ProducerReturned() || !frame->policy->ConsumerReturned() ||
            !producer.Valid() || !consumer.Valid() || producer.list != frame->producer.list ||
            consumer.list != frame->consumer.list || producer.generation <= frame->producer.generation ||
            consumer.generation <= frame->consumer.generation) return false;
        for (const auto& call : _calls)
            if (call.receipt.Valid())
                for (uint32_t i = 0; i < call.count; ++i)
                    if (call.entries[i].index == key.index) return false;
        const auto found = std::find(_watches.begin(), _watches.end(), key.index);
        if (found == _watches.end()) return false;
        _watches.erase(found);
        if (Continuous()) Clear(key.index); // Old immutable key can no longer match this slot.
        return true;
    }
    // A scene change may leave one successfully submitted look-ahead producer
    // with no denoiser consumer. This does NOT acknowledge denoiser history.
    // Host separately proves all producer/private-copy fences and no CPU readers.
    bool RetireUnusedProducer(FrameKey key, Recording producer, Recording consumer = {}) noexcept
    {
        const auto* frame = FindConst(key);
        if (!Continuous() || !_stopped || !frame || frame->policy->Failed() ||
            frame->policy->ConsumerEmbedded() || !frame->policy->ProducerReturned() ||
            !producer.Valid() || producer.list != frame->producer.list ||
            producer.generation <= frame->producer.generation || frame->consumer.Valid()) return false;
        if (consumer.list) return false; // No untracked consumer recording is accepted.
        for (const auto& call : _calls)
            if (call.receipt.Valid())
                for (uint32_t i = 0; i < call.count; ++i)
                    if (call.entries[i].index == key.index) return false;
        const auto found = std::find(_watches.begin(), _watches.end(), key.index);
        if (found == _watches.end()) return false;
        _watches.erase(found); Clear(key.index);
        return true;
    }
    size_t SubmissionWatches() const noexcept { return _watches.size(); }

    // Admission/preparation can fail before any private command was recorded.
    // This is not cancellation of submitted work: host must independently prove
    // no producer/guide/copy/consumer recording, ticket, or CPU callback exists.
    bool RetireUnrecordedFrame(FrameKey key, bool noGpuRecording) noexcept
    {
        const auto* frame = FindConst(key);
        if (!Continuous() || !_stopped || !noGpuRecording || !frame ||
            frame->policy->ProducerDeclared() || frame->policy->ConsumerEmbedded() ||
            frame->producer.list || frame->consumer.list) return false;
        for (const auto& call : _calls)
            if (call.receipt.Valid())
                for (uint32_t i = 0; i < call.count; ++i)
                    if (call.entries[i].index == key.index) return false;
        const auto found = std::find(_watches.begin(), _watches.end(), key.index);
        if (found == _watches.end()) return false;
        _watches.erase(found); Clear(key.index);
        return true;
    }

  private:
    static constexpr uint8_t RoleBit(Role role) noexcept
    { return role == Role::Ray ? 1 : role == Role::Guides ? 2 : role == Role::Fog ? 4 : 0; }
    static constexpr uint8_t ProducerRoles = 3;
    struct Frame
    {
        std::optional<FramePolicy::Policy> policy;
        Recording producer {}, consumer {};
        uint8_t roles = 0;
        uint32_t index = UINT32_MAX;
    };
    struct Call
    {
        struct Entry { uint32_t index = 0; uint64_t token = 0; };
        Receipt receipt;
        std::array<Entry, MaxFramesPerCall> entries {};
        uint32_t count = 0;
    };
    FrameKey Key(uint32_t index) const noexcept { return { _epoch, index, _first + index, _frameCount }; }
    Frame& At(uint32_t index) noexcept { return _frames[index % _frames.size()]; }
    const Frame& At(uint32_t index) const noexcept { return _frames[index % _frames.size()]; }
    void Clear(uint32_t index) noexcept
    {
        auto& frame = At(index);
        frame.policy.reset(); frame.producer = {}; frame.consumer = {};
        frame.roles = 0; frame.index = UINT32_MAX;
    }
    const Frame* FindConst(FrameKey key) const noexcept
    {
        return _valid && key.Valid() && key.epoch == _epoch && key.frameCount == _frameCount &&
            key.index <= UINT32_MAX - _first && key.frame == _first + key.index &&
            At(key.index).index == key.index && At(key.index).roles ? &At(key.index) : nullptr;
    }
    Frame* Find(FrameKey key) noexcept
    {
        if (const auto* frame = FindConst(key)) return const_cast<Frame*>(frame);
        StopWith(Failure::InvalidFrame);
        return nullptr;
    }
    void StopWith(Failure failure) noexcept
    {
        _stopped = true;
        if (_failure == Failure::None) _failure = failure;
    }
    bool MissingRole() noexcept { StopWith(Failure::MissingRole); return false; }
    bool RefuseFrame(Frame& frame) noexcept
    { frame.policy->Fail(); StopWith(Failure::FramePolicyRefused); return false; }
    SubmissionDecision RejectSubmission(Failure failure,
        FramePolicy::Failure frameFailure = FramePolicy::Failure::None) noexcept
    {
        // A partly mutated/missed pre-submit observation is not recoverable.
        // Preserve every obligation and poison started policies; host must veto
        // the unsafe native call rather than drop lists or continue best-effort.
        for (auto& frame : _frames) if (frame.roles) frame.policy->Fail();
        StopWith(failure);
        return { false, failure, frameFailure };
    }
    ReturnDecision RejectReturn() noexcept
    {
        RejectSubmission(Failure::InvalidReturnReceipt);
        return { false, Failure::InvalidReturnReceipt };
    }

    uint64_t _epoch = 0, _nextCall = 0;
    Queue _queue {};
    uint32_t _first = 0, _committed = 0, _frameCount = FrameCount;
    bool _valid = false, _stopped = false;
    Failure _failure = Failure::None;
    std::vector<Frame> _frames;
    std::vector<uint32_t> _watches;
    std::array<Call, MaxPendingCalls> _calls {};
};
} // namespace FSRD::CyberpunkTemporalWindowPolicy
