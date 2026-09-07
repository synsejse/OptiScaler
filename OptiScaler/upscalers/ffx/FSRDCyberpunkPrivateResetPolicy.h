#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace FSRD::CyberpunkPrivateResetPolicy
{
// One policy object per immutable, preallocated packet. The host serializes ALL
// methods, including before/after-Execute callbacks, with its packet mutex. No
// lock may be held across the original Execute call. This class neither waits
// nor calls D3D/engine code, allocates, retains resources, or authenticates frames.
struct Recording
{
    uintptr_t list = 0; // Canonical ID3D12CommandList identity, not a sampled alias.
    uint64_t generation = 0; // Actual observed Reset generation; zero is unknown.
    constexpr bool Valid() const noexcept { return list && generation; }
    bool operator==(const Recording&) const = default;
};

struct Queue
{
    uintptr_t queue = 0, device = 0; // Actual canonical identities at this call.
    bool direct = false;
    bool operator==(const Queue&) const = default;
};

struct ProducerSeal
{
    Recording ray, guides;
    // Ordinals describe authenticated insertion points on ONE native recording,
    // not frame IDs, global callback counters or estimated queue execution order.
    uint64_t rayTerminal = 0, guideBegin = 0, guideTerminal = 0, sealed = 0;
    bool raySucceeded = false, guidesSucceeded = false;
    bool rayReadBarriers = false, guideReadBarriers = false;
    bool rayRestored = false, guidesRestored = false;
    bool rayOwnersRetained = false, guideOwnersRetained = false;
};

enum class Admission : uint8_t
{
    Unrelated,
    ProducerOnly,
    SameRecording,
    EarlierInArray,
    ReturnedProducer,
    Refused
};

enum class Failure : uint8_t
{
    None,
    InvalidPacket,
    InvalidRecording,
    RepeatedRole,
    ProducerNotDeclared,
    InvalidProducerSeal,
    ConsumerOwnerMissing,
    InvalidConsumerSeal,
    GenerationChanged,
    ArrayTooLarge,
    DuplicateEntry,
    WrongQueue,
    ProducerNotSealed,
    ProducerAlreadySubmitted,
    ConsumerAlreadySubmitted,
    ConsumerNotSealed,
    ProducerCallNotReturned,
    MissingProducerSubmission,
    WrongLocalOrder,
    WrongArrayOrder,
    FailedPacket,
    InvalidReturnToken
};

struct Decision
{
    Admission admission = Admission::Refused;
    Failure failure = Failure::None;
    uint64_t token = 0; // Nonzero only when this original call must be reported.
    constexpr bool Allowed() const noexcept { return admission != Admission::Refused; }
};

class Policy
{
  public:
    static constexpr size_t MaxExecuteLists = 256; // Diagnostic bound, not a D3D limit.

    explicit Policy(uintptr_t canonicalDevice) noexcept : _device(canonicalDevice)
    {
        if (!_device) Poison(Failure::InvalidPacket);
    }
    Policy(const Policy&) = delete;
    Policy& operator=(const Policy&) = delete;
    Policy(Policy&&) = delete;
    Policy& operator=(Policy&&) = delete;

    // Call before the first producer target command, so a producer submission
    // cannot escape observation while the consumer is still being recorded.
    bool DeclareProducer(Recording recording) noexcept
    {
        if (_failed) return false;
        if (!recording.Valid()) return Poison(Failure::InvalidRecording);
        if (_producerDeclared) return Poison(Failure::RepeatedRole);
        if (_consumerEmbedded && recording.list == _consumer.list && recording != _consumer)
            return Poison(Failure::GenerationChanged);
        _producer = recording;
        _producerDeclared = true;
        return true;
    }

    // Must follow ALL producer commands, including any optional balanced
    // readbacks. The host proves exact targets, frame/camera/extent/encoding,
    // actual terminal transitions and native restoration before making this
    // immutable declaration. Allocation/positive refs are not readiness proof.
    bool SealProducer(const ProducerSeal& seal) noexcept
    {
        if (_failed) return false;
        if (!_producerDeclared) return Poison(Failure::ProducerNotDeclared);
        if (_producerSealed) return Poison(Failure::RepeatedRole);
        if (seal.ray != _producer || seal.guides != _producer)
            return Poison(Failure::GenerationChanged);
        if (!seal.rayTerminal || seal.rayTerminal >= seal.guideBegin ||
            seal.guideBegin > seal.guideTerminal || seal.guideTerminal > seal.sealed ||
            !seal.raySucceeded || !seal.guidesSucceeded || !seal.rayReadBarriers || !seal.guideReadBarriers ||
            !seal.rayRestored || !seal.guidesRestored || !seal.rayOwnersRetained || !seal.guideOwnersRetained)
            return Poison(Failure::InvalidProducerSeal);
        _producerSealed = true;
        _producerTerminal = seal.sealed;
        return true;
    }

    // Publish before even the first potentially failing consumer dispatch. A
    // subsequent Record=false/throw/timeout must call Fail(), never remove the
    // obligation. The host independently owns the exact packet and consumer
    // resources before any GPU references; this boolean is a checked receipt.
    bool EmbedConsumer(Recording recording, uint64_t firstReadOrdinal, bool ownersRetained) noexcept
    {
        if (_failed) return false;
        if (!recording.Valid() || !firstReadOrdinal) return Poison(Failure::InvalidRecording);
        if (_consumerEmbedded) return Poison(Failure::RepeatedRole);
        if (!ownersRetained) return Poison(Failure::ConsumerOwnerMissing);
        if (_producerDeclared && recording.list == _producer.list && recording != _producer)
            return Poison(Failure::GenerationChanged);
        _consumer = recording;
        _consumerFirstRead = firstReadOrdinal;
        _consumerEmbedded = true;
        return true;
    }

    bool SealConsumer(Recording recording, uint64_t terminalOrdinal, bool succeeded, bool restored) noexcept
    {
        if (_failed) return false;
        if (!_consumerEmbedded || _consumerSealed) return Poison(Failure::RepeatedRole);
        if (recording != _consumer) return Poison(Failure::GenerationChanged);
        if (!terminalOrdinal || terminalOrdinal < _consumerFirstRead || !succeeded || !restored)
            return Poison(Failure::InvalidConsumerSeal);
        _consumerSealed = true;
        return true;
    }

    void Fail() noexcept { Poison(Failure::FailedPacket); }
    bool ConsumerEmbedded() const noexcept { return _consumerEmbedded; }
    bool ConsumerAdmitted() const noexcept { return _consumerSubmitted; }
    bool ConsumerReturned() const noexcept { return _consumerReturned; }
    bool ProducerReturned() const noexcept { return _producerReturned; }
    bool ProducerDeclared() const noexcept { return _producerDeclared; }
    bool Failed() const noexcept { return _failed; }
    Failure LastFailure() const noexcept { return _failure; }

    // Observe from the instant the packet is armed, BEFORE native Execute and
    // before any host submission-ticket bookkeeping that assumes submission.
    // Entries must describe this call's actual lists/current Reset generations.
    // Unknown unrelated list generations are harmless; unknown packet-list
    // generations are not. Do not fabricate old generations from role receipts.
    Decision BeforeExecute(Queue queue, std::span<const Recording> lists) noexcept
    {
        if (lists.empty()) return { Admission::Unrelated };
        // An oversized unexamined array cannot be certified unrelated. Poison
        // this one diagnostic packet, retaining any embedded-read obligation;
        // never silently continue its future admission after a missed call.
        if (lists.size() > MaxExecuteLists) return Reject(Failure::ArrayTooLarge);
        size_t producerIndex = lists.size(), consumerIndex = lists.size();
        bool changed = false;
        for (size_t i = 0; i < lists.size(); ++i)
        {
            const auto current = lists[i];
            if (_producerDeclared && current.list == _producer.list)
            {
                if (!current.Valid()) changed = true;
                else if (current == _producer) producerIndex = i;
                // A returned immutable producer receipt survives reusable-list
                // Reset. New-generation work contains none of our old commands.
                else if (!_producerReturned) changed = true;
            }
            if (_consumerEmbedded && current.list == _consumer.list)
            {
                if (!current.Valid()) changed = true;
                else if (current == _consumer) consumerIndex = i;
                else if (!_consumerReturned) changed = true;
            }
        }
        const bool hasProducer = producerIndex != lists.size(), hasConsumer = consumerIndex != lists.size();
        if (changed) return Reject(Failure::GenerationChanged);
        if (!hasProducer && !hasConsumer) return { Admission::Unrelated };
        if (_failed) return { Admission::Refused, _failure };
        for (size_t i = 0; i < lists.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                if (lists[i].list && lists[i].list == lists[j].list)
                    return Reject(Failure::DuplicateEntry);
        if (!queue.queue || !queue.direct || queue.device != _device || (_queue.queue && queue != _queue))
            return Reject(Failure::WrongQueue);
        if (!_producerSealed) return Reject(Failure::ProducerNotSealed);
        if (hasProducer && _producerSubmitted) return Reject(Failure::ProducerAlreadySubmitted);
        if (hasConsumer && _consumerSubmitted) return Reject(Failure::ConsumerAlreadySubmitted);
        if (hasConsumer && !_consumerSealed) return Reject(Failure::ConsumerNotSealed);

        Admission admission = Admission::ProducerOnly;
        if (hasConsumer)
        {
            if (_consumer == _producer)
            {
                if (_producerTerminal >= _consumerFirstRead) return Reject(Failure::WrongLocalOrder);
                admission = Admission::SameRecording;
            }
            else if (hasProducer)
            {
                if (producerIndex >= consumerIndex) return Reject(Failure::WrongArrayOrder);
                admission = Admission::EarlierInArray;
            }
            else
            {
                if (_producerSubmitted && !_producerReturned) return Reject(Failure::ProducerCallNotReturned);
                if (!_producerReturned) return Reject(Failure::MissingProducerSubmission);
                admission = Admission::ReturnedProducer;
            }
        }
        _queue = queue;
        const uint64_t token = ++_nextToken; // At most two related admissions for one packet.
        if (hasProducer) { _producerSubmitted = true; _producerToken = token; }
        if (hasConsumer) { _consumerSubmitted = true; _consumerToken = token; }
        return { admission, Failure::None, token };
    }

    // Only AFTER the exact original Execute invocation has returned normally.
    // Entry serials, preparing/submitted flags and GPU-fence guesses cannot call
    // this in advance. Unrelated decisions have no return obligation/token.
    bool AfterExecute(uint64_t token) noexcept
    {
        const bool producer = token && token == _producerToken && !_producerReturned;
        const bool consumer = token && token == _consumerToken && !_consumerReturned;
        if (!producer && !consumer) return Poison(Failure::InvalidReturnToken);
        if (producer) _producerReturned = true;
        if (consumer) _consumerReturned = true;
        return true;
    }

  private:
    bool Poison(Failure failure) noexcept
    {
        if (!_failed) _failure = failure;
        _failed = true;
        return false;
    }
    Decision Reject(Failure failure) noexcept
    {
        Poison(failure);
        return { Admission::Refused, _failure };
    }

    uintptr_t _device = 0;
    Queue _queue {};
    Recording _producer {}, _consumer {};
    uint64_t _producerTerminal = 0, _consumerFirstRead = 0;
    uint64_t _nextToken = 0, _producerToken = 0, _consumerToken = 0;
    bool _producerDeclared = false, _producerSealed = false, _consumerEmbedded = false, _consumerSealed = false;
    bool _producerSubmitted = false, _producerReturned = false;
    bool _consumerSubmitted = false, _consumerReturned = false;
    bool _failed = false;
    Failure _failure = Failure::None;
};
} // namespace FSRD::CyberpunkPrivateResetPolicy
