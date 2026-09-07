#include "pch.h"
#include "FSRDCyberpunkEarlyGuides.h"
#include "FSRDCyberpunkMotionScale.h"

#include <Windows.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace FSRDCyberpunkEarlyGuides
{
namespace
{
using Json = nlohmann::json;
constexpr size_t MaxReadCalls = 4096, MaxReadBytes = 128 * 1024;
constexpr uint32_t MaxChain = 256, MaxVersions = 256, MaxTableElements = 1024 * 1024;
constexpr uint32_t MaxStride = 256;
constexpr uintptr_t ImageBytes = 0x04efc000;
constexpr uintptr_t TextureRegistryRva = 0x3438a28;
constexpr uint32_t TextureSlotCount = 0x8000;
constexpr uintptr_t TextureSlotStride = 0xb0, TextureRefOffset = 0x2f1d0,
                    TextureNativeOffset = 0x2f1d8;
constexpr uintptr_t NoVModeRva = 0x38137f0, ExtraSpecularEnableRva = 0x3310670,
                    ExtraSpecularScaleRva = 0x3813840;

uintptr_t Address(uintptr_t base, uint64_t offset)
{
    if (!base || offset > std::numeric_limits<uintptr_t>::max() - base)
        throw std::runtime_error("null or overflowing address");
    return base + static_cast<uintptr_t>(offset);
}

struct Reader
{
    size_t calls = 0, bytes = 0;

    template <typename T> T Read(uintptr_t address)
    {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= MaxReadBytes);
        if (!address || sizeof(T) - 1 > std::numeric_limits<uintptr_t>::max() - address)
            throw std::runtime_error("invalid read range");
        if (calls >= MaxReadCalls || bytes > MaxReadBytes - sizeof(T))
            throw std::runtime_error("CPU metadata read budget exhausted");
        ++calls;
        bytes += sizeof(T);
        T result {};
        SIZE_T actual = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &result,
                               sizeof(result), &actual) || actual != sizeof(result))
            throw std::runtime_error("CPU metadata memory unavailable");
        return result;
    }
};

// Relative to the native-pointer slot (registry+0x2f1d8+index*0xb0).
// 0x1f3a6c uses +0x30; t4's special 0x774be0 uses +0x38 instead.
// Factory 0x2221f4 reads the compact descriptor at +0x4e. These are CPU
// descriptor-handle integers/raw engine fields, not readable descriptors.
Json DescriptorSources(Reader& read, uintptr_t nativeSlot)
{
    Json result = { { "status", "unavailable" }, { "descriptor_handles_dereferenced", false },
        { "descriptor_contents", "not_observed" }, { "usable_srv", "not_established" },
        { "native_view_format", "not_observed" }, { "resource_state", "not_observed" },
        { "snapshot_atomic", false } };
    try
    {
        const auto descriptors = read.Read<std::array<uintptr_t, 2>>(Address(nativeSlot, 0x30));
        const auto requestedState = read.Read<uint32_t>(Address(nativeSlot, 0x48));
        const auto compact = read.Read<std::array<uint8_t, 12>>(Address(nativeSlot, 0x4e));
        const bool unchanged = descriptors == read.Read<std::array<uintptr_t, 2>>(Address(nativeSlot, 0x30)) &&
            requestedState == read.Read<uint32_t>(Address(nativeSlot, 0x48)) &&
            compact == read.Read<std::array<uint8_t, 12>>(Address(nativeSlot, 0x4e));
        result["repeated_source_fields_equal"] = unchanged;
        if (!unchanged)
            throw std::runtime_error("texture descriptor-source fields changed during reads");
        result["ordinary_cpu_srv_handle"] = descriptors[0];
        result["alternate_cpu_srv_handle"] = descriptors[1];
        result["requested_srv_state_mask"] = requestedState;
        result["requested_state_semantics"] = "engine_SRV_request_mask_not_current_state";
        result["raw_compact_descriptor_bytes"] = compact;
        result["raw_array_size"] = uint32_t(compact[4]) | (uint32_t(compact[5]) << 8);
        result["raw_dimension_mip_bits"] = compact[6];
        result["raw_format_sample_bits"] = compact[7];
        result["raw_flags_bits"] = uint32_t(compact[8]) | (uint32_t(compact[9]) << 8) |
            (uint32_t(compact[10]) << 16) | (uint32_t(compact[11]) << 24);
        // Zero, reserved bits and unsupported formats remain raw evidence.
        // Do not turn creation-branch conditions into a live view claim.
        result["status"] = "cpu_descriptor_sources_observed";
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

// RVA 0x20d268 only borrows a native address. RVA 0x21f980 establishes
// the actual 32768-slot bound; 0x21c960/0x1fa164 increment/decrement the
// signed slot refcount. Zero can already be queued for retirement. This
// records addresses as integers only: never dereference them or invoke COM.
// Repeated matching reads detect some mutations, not lifetime/ownership.
Json RegistryMapping(Reader& read, uintptr_t image, uint32_t handle)
{
    Json result = { { "status", "unavailable" }, { "slot_capacity", TextureSlotCount },
        { "native_address_borrowed", true }, { "native_address_dereferenced", false },
        { "lifetime", "not_established" }, { "gpu_initialized", "not_established" },
        { "resource_state", "not_observed" }, { "snapshot_atomic", false } };
    try
    {
        if (!handle || handle > TextureSlotCount)
            throw std::runtime_error("native handle outside authenticated texture pool");
        const auto registry = read.Read<uintptr_t>(Address(image, TextureRegistryRva));
        const auto index = uint64_t(handle - 1);
        result["slot_index"] = index;
        const auto refsAddress = Address(registry, TextureRefOffset + index * TextureSlotStride);
        const auto nativeAddress = Address(registry, TextureNativeOffset + index * TextureSlotStride);
        const auto refs = read.Read<int32_t>(refsAddress);
        result["ref_status"] = refs;
        if (refs <= 0)
            throw std::runtime_error("texture slot has no observed positive reference count");
        const auto native = read.Read<uintptr_t>(nativeAddress);
        if (!native)
            throw std::runtime_error("native resource address unavailable");
        auto descriptors = DescriptorSources(read, nativeAddress);
        const auto refsAfter = read.Read<int32_t>(refsAddress);
        result["ref_status_after"] = refsAfter;
        if (refs != refsAfter || native != read.Read<uintptr_t>(nativeAddress) ||
            registry != read.Read<uintptr_t>(Address(image, TextureRegistryRva)))
            throw std::runtime_error("texture registry changed during metadata reads");
        result["borrowed_native_address"] = native;
        result["descriptor_sources"] = std::move(descriptors);
        result["status"] = "borrowed_address_observed";
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

struct GraphContext
{
    uintptr_t context = 0, view = 0, graph = 0;
    uint8_t flags = 0, selector = 0;
    uint32_t nameSpace = 0, counter = 0, position = 0;
};

GraphContext ReadContext(Reader& read, uintptr_t context)
{
    GraphContext result;
    result.context = context;
    result.flags = read.Read<uint8_t>(Address(context, 0x30));
    if (!(result.flags & 2))
        throw std::runtime_error("not an executing graph context");
    result.view = read.Read<uintptr_t>(Address(context, 0x18));
    if (!result.view)
        throw std::runtime_error("current view unavailable");
    result.nameSpace = result.flags & 1 ? 0 : read.Read<uint32_t>(Address(context, 0x34));
    result.selector = read.Read<uint8_t>(Address(context, 0x38));
    const auto wrapper = read.Read<uintptr_t>(Address(context, 0x08));
    result.graph = read.Read<uintptr_t>(Address(wrapper, 0));
    result.counter = read.Read<uint32_t>(Address(result.graph, uint64_t(result.selector) * 0x5e00 + 0x40));
    // The original narrows to uint16; refuse unsupported wraparound instead.
    if (result.counter > 65536)
        throw std::runtime_error("graph version position exceeds supported range");
    result.position = (uint32_t(result.selector) << 16) | (std::max(result.counter, 1u) - 1);
    return result;
}

struct TableHeader
{
    uintptr_t buckets;
    uint32_t count, bucketCount;
    uintptr_t entries;
    uint32_t opaque, stride;
};
static_assert(sizeof(uintptr_t) == 8 && sizeof(TableHeader) == 32);

// Compiler 0x24cac8 and allocator 0x1f7f24 use closed unsigned ranges of
// (selector << 16) | uint16(operationIndex), stored in uint64 fields.
// 0x24d114 preserves the original end at +0x10 BEFORE +0x08 is extended
// through protected scopes. This describes compiler reservation only:
// neither a producer, native resource lifetime, nor GPU readiness is proved.
Json LogicalInterval(Reader& read, const GraphContext& context, uintptr_t holder,
                     uintptr_t resourceRecord, uint32_t handle)
{
    constexpr uintptr_t HolderArenaOffset = 0x514a18, HolderStride = 0x58;
    constexpr uint32_t HolderCapacity = 320, MaxEncodedPosition = 0x00ffffff;
    Json result = { { "status", "unavailable" }, { "holder_address", holder },
        { "resource_record_address", resourceRecord }, { "graph_address", context.graph },
        { "current_position", context.position }, { "current_operation_counter", context.counter },
        { "current_selector", context.selector }, { "holder_capacity", HolderCapacity },
        { "position_encoding", "selector_u8_shift16_or_operation_u16" },
        { "interval_semantics", "inclusive_compiler_reservation_only" },
        { "producer_completion", "not_established" }, { "alias_lifetime", "not_established" },
        { "resource_state", "not_observed" }, { "snapshot_atomic", false } };
    try
    {
        const auto phase = read.Read<uint8_t>(Address(context.graph, 2));
        const auto count = read.Read<uint32_t>(Address(context.graph, 0x514a10));
        result["graph_phase"] = phase;
        result["holder_count"] = count;
        const auto arena = Address(context.graph, HolderArenaOffset);
        if (!count || count > HolderCapacity || holder < arena ||
            (holder - arena) % HolderStride || (holder - arena) / HolderStride >= count)
            throw std::runtime_error("holder outside current bounded graph arena");
        result["holder_index"] = (holder - arena) / HolderStride;
        const auto ranges = read.Read<std::array<uint64_t, 3>>(holder);
        const auto recordRanges = read.Read<std::array<uint64_t, 2>>(resourceRecord);
        const auto used = read.Read<uint8_t>(Address(resourceRecord, 0x10));
        const auto kind = read.Read<uint8_t>(Address(resourceRecord, 0x3c));
        const auto policy = read.Read<uint8_t>(Address(resourceRecord, 0x40));
        result["holder_first_use"] = ranges[0];
        result["holder_reservation_end"] = ranges[1];
        result["holder_end_event_position"] = ranges[2];
        // A reused physical record may describe another logical reservation.
        // Do not substitute its ranges for the selected holder's interval.
        result["record_range_begin"] = recordRanges[0];
        result["record_range_end"] = recordRanges[1];
        result["record_used_flag"] = used;
        result["record_handle"] = handle;
        result["record_kind"] = kind;
        result["record_policy"] = policy;
        const bool unchanged = ranges == read.Read<std::array<uint64_t, 3>>(holder) &&
            recordRanges == read.Read<std::array<uint64_t, 2>>(resourceRecord) &&
            resourceRecord == read.Read<uintptr_t>(Address(holder, 0x50)) &&
            handle == read.Read<uint32_t>(Address(resourceRecord, 0x14)) &&
            used == read.Read<uint8_t>(Address(resourceRecord, 0x10)) &&
            kind == read.Read<uint8_t>(Address(resourceRecord, 0x3c)) &&
            policy == read.Read<uint8_t>(Address(resourceRecord, 0x40)) &&
            count == read.Read<uint32_t>(Address(context.graph, 0x514a10)) &&
            phase == read.Read<uint8_t>(Address(context.graph, 2)) &&
            context.counter == read.Read<uint32_t>(Address(context.graph,
                uint64_t(context.selector) * 0x5e00 + 0x40)) &&
            context.selector == read.Read<uint8_t>(Address(context.context, 0x38));
        result["repeated_metadata_equal"] = unchanged;
        if (!unchanged)
            throw std::runtime_error("graph interval metadata changed during reads");
        if (phase != 2 || !context.counter || used != 1)
            throw std::runtime_error("executing operation and assigned record not established");
        if (ranges[0] > MaxEncodedPosition || ranges[1] > MaxEncodedPosition ||
            ranges[2] > MaxEncodedPosition || ranges[0] > ranges[2] || ranges[2] > ranges[1])
            throw std::runtime_error("unsupported or unordered compiler interval");
        result["inclusive_contains_position"] =
            ranges[0] <= context.position && context.position <= ranges[1];
        result["end_event_relation"] = context.position < ranges[2] ? "before" :
            context.position == ranges[2] ? "at" : "after";
        result["status"] = "compiler_interval_observed";
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

// Bounded read-only translation of RVA 0x1f3d20 and version selector 0x96d5ec
// for the already-authenticated executable. Missing versions fail closed, unlike
// the original helper's unchecked dereference. No graph dependency registration.
Json Resolve(Reader& read, const GraphContext& context, uint32_t baseKey, const char* name)
{
    const uint32_t key = baseKey ^ (context.nameSpace << 24);
    Json result = { { "input", name }, { "base_key", baseKey }, { "namespaced_key", key },
                    { "status", "unavailable" }, { "gpu_initialized", "not_established" } };
    try
    {
        const auto table = read.Read<TableHeader>(Address(context.graph, 0x51b848));
        result["table_count"] = table.count;
        result["table_bucket_count"] = table.bucketCount;
        result["table_stride"] = table.stride;
        result["entry_allocation_capacity"] = "not_observed";
        if (!table.count)
        {
            result["reason"] = "empty graph table";
            return result;
        }
        if (table.count > MaxTableElements || !table.bucketCount || table.bucketCount > MaxTableElements ||
            table.stride < 0x38 || table.stride > MaxStride)
            throw std::runtime_error("unsupported graph table bounds");
        uint32_t index = read.Read<uint32_t>(Address(table.buckets, uint64_t(key % table.bucketCount) * 4));
        std::array<uint32_t, MaxChain> visited {};
        uintptr_t entry = 0;
        for (uint32_t step = 0; index != UINT32_MAX; ++step)
        {
            // count is not authenticated as allocation capacity. This is only a
            // traversal/address bound; RPM remains the actual memory-read guard.
            if (step == MaxChain || index >= MaxTableElements ||
                std::find(visited.begin(), visited.begin() + step, index) != visited.begin() + step)
                throw std::runtime_error("graph hash chain bound or cycle");
            visited[step] = index;
            const auto candidate = Address(table.entries, uint64_t(index) * table.stride);
            const auto keys = read.Read<std::array<uint32_t, 3>>(candidate);
            if (keys[1] == key && keys[2] == key)
            {
                entry = candidate;
                result["table_entry_index"] = index;
                result["chain_steps"] = step + 1;
                break;
            }
            index = keys[0];
        }
        if (!entry)
        {
            result["reason"] = "key absent from current graph table";
            return result;
        }
        uintptr_t holder = read.Read<uintptr_t>(Address(entry, 0x20));
        if (holder)
            result["selection"] = "fixed_resource";
        else
        {
            result["selection"] = "versioned_resource";
            const auto versions = read.Read<uintptr_t>(Address(entry, 0x28));
            const auto count = read.Read<uint32_t>(Address(entry, 0x34));
            result["version_count"] = count;
            if (!count || count > MaxVersions)
                throw std::runtime_error("empty or unsupported version array");
            bool selected = false;
            uint64_t previous = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto boundary = read.Read<uint64_t>(Address(versions, uint64_t(i) * 24 + 8));
                if (i && boundary < previous)
                    throw std::runtime_error("nonmonotonic version boundaries");
                previous = boundary;
                if (boundary >= context.position)
                {
                    const auto selectedIndex = i ? i - 1 : 0;
                    holder = read.Read<uintptr_t>(Address(versions, uint64_t(selectedIndex) * 24));
                    result["selected_version_index"] = selectedIndex;
                    result["selection_boundary_index"] = i;
                    result["selection_boundary"] = boundary;
                    selected = true;
                    break;
                }
            }
            if (!selected)
                throw std::runtime_error("no version boundary for current graph position");
        }
        if (!holder)
            throw std::runtime_error("selected resource holder unavailable");
        const auto resourceRecord = read.Read<uintptr_t>(Address(holder, 0x50));
        if (!resourceRecord)
            throw std::runtime_error("selected resource record unavailable");
        const auto handle = read.Read<uint32_t>(Address(resourceRecord, 0x14));
        result["handle"] = handle;
        result["logical_interval"] = LogicalInterval(read, context, holder, resourceRecord, handle);
        result["status"] = handle && handle <= INT32_MAX ? "handle_present" : "unavailable";
        if (!handle || handle > INT32_MAX)
            result["reason"] = "selected native handle invalid";
    }
    catch (const std::exception& error)
    {
        result["reason"] = error.what();
    }
    return result;
}

// RVA 0x23af5c: two locally snapshotted feature words cover the four exact
// bits used below. This is not an atomic snapshot of the engine's whole view.
struct ViewFeatures
{
    struct Word { uint64_t value = 0; bool known = false; std::string reason; };
    std::array<Word, 2> words;

    ViewFeatures(Reader& read, const GraphContext& context)
    {
        for (size_t i = 0; i < words.size(); ++i)
        {
            try
            {
                words[i].value = context.flags & 1 ? 0 :
                    read.Read<uint64_t>(Address(context.view, 0x17d0 + i * 8));
                words[i].known = true;
            }
            catch (const std::exception& error) { words[i].reason = error.what(); }
        }
    }

    bool Test(uint32_t bit) const
    {
        if (bit / 64 >= words.size() || !words[bit / 64].known)
            throw std::runtime_error("current view feature unavailable");
        return ((words[bit / 64].value >> (bit & 63)) & 1) != 0;
    }

    Json Describe() const
    {
        Json result = Json::array();
        for (size_t i = 0; i < words.size(); ++i)
        {
            Json item = { { "view_offset", 0x17d0 + i * 8 }, { "known", words[i].known } };
            if (words[i].known) item["bits"] = words[i].value;
            else item["reason"] = words[i].reason;
            result.push_back(std::move(item));
        }
        return result;
    }
};

void SetHandle(Json& result, uint32_t handle)
{
    result["handle"] = handle;
    result["status"] = handle && handle <= INT32_MAX ? "handle_present" : "unavailable";
    if (!handle || handle > INT32_MAX) result["reason"] = "selected native handle invalid";
}

Json ExtraSpecular(Reader& read, const GraphContext& context, const ViewFeatures& features)
{
    Json result = { { "input", "t5_extra_specular" }, { "status", "unavailable" },
                    { "gpu_initialized", "not_established" } };
    try
    {
        const bool enabled = features.Test(0x35);
        result["feature_0x35"] = enabled;
        if (!enabled)
        {
            result["status"] = "not_enabled_by_current_view";
            return result;
        }
        const auto owner = read.Read<uintptr_t>(Address(context.view, 0x1d70));
        const auto handle = read.Read<uint32_t>(Address(owner, 0x2e8));
        result["handle"] = handle;
        result["status"] = handle && handle <= INT32_MAX ? "handle_present" : "unavailable";
        if (!handle || handle > INT32_MAX)
            result["reason"] = "optional native handle invalid";
    }
    catch (const std::exception& error)
    {
        result["reason"] = error.what();
    }
    return result;
}

// ApplyDLSS RVA 0x37d6b6-0x37d710 chooses the graph fallback first, then
// replaces it with EVERY nonzero owner handle (not merely valid handles).
Json Motion(Reader& read, const GraphContext& context, const ViewFeatures& features)
{
    Json result = { { "input", "motion_vectors" }, { "status", "unavailable" },
                    { "gpu_initialized", "not_established" }, { "streamline_tag", 1 } };
    Json fallback = { { "status", "unavailable" } };
    try
    {
        const bool enabled = features.Test(0x5a);
        result["feature_0x5a"] = enabled;
        if (enabled) fallback = Resolve(read, context, 0x15eab19c, "motion_graph_fallback");
        else fallback = { { "status", "not_enabled_by_current_view" }, { "handle", 0 } };
    }
    catch (const std::exception& error) { fallback["reason"] = error.what(); }
    result["graph_fallback"] = fallback;
    try
    {
        const bool feature35 = features.Test(0x35), feature37 = features.Test(0x37);
        result["feature_0x35"] = feature35;
        result["feature_0x37"] = feature37;
        if (feature35 || feature37)
        {
            const auto owner = read.Read<uintptr_t>(Address(context.view, 0x1d70));
            const auto overrideHandle = read.Read<uint32_t>(Address(owner, 0x268));
            result["owner_override_handle"] = overrideHandle;
            if (overrideHandle)
            {
                result["selected_source"] = "current_view_owner_0x268";
                SetHandle(result, overrideHandle);
                return result;
            }
        }
        if (!fallback.contains("handle"))
            throw std::runtime_error("motion fallback unavailable and no proven nonzero owner override");
        result["selected_source"] = "graph_fallback_or_zero";
        SetHandle(result, fallback["handle"].get<uint32_t>());
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

Json SpecularHitDistance(Reader& read, const GraphContext& context, const ViewFeatures& features)
{
    Json result = { { "input", "specular_hit_distance" }, { "status", "unavailable" },
                    { "gpu_initialized", "not_established" }, { "streamline_tag", 42 } };
    try
    {
        const bool enabled = features.Test(0x46);
        result["feature_0x46"] = enabled;
        if (!enabled)
        {
            result["status"] = "not_enabled_by_current_view";
            return result;
        }
        const auto owner = read.Read<uintptr_t>(Address(context.view, 0x1d70));
        result["selected_source"] = "current_view_owner_0x274";
        SetHandle(result, read.Read<uint32_t>(Address(owner, 0x274)));
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

Json FloatField(Reader& read, uintptr_t view, uintptr_t offset, uint32_t signXor = 0)
{
    Json result = { { "view_offset", offset }, { "byte_size", sizeof(float) },
        { "producer_sign_xor", signXor }, { "status", "unavailable" } };
    try
    {
        const auto bits = read.Read<uint32_t>(Address(view, offset));
        const auto candidateBits = bits ^ signXor;
        float candidate = 0;
        std::memcpy(&candidate, &candidateBits, sizeof(candidate));
        result["source_bits"] = bits;
        result["producer_candidate_bits"] = candidateBits;
        result["producer_candidate"] = std::isfinite(candidate) ? Json(candidate) : Json(nullptr);
        result["status"] = "CPU_value_present";
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

// One complete bounded read per raw block; a repeated read detects some source
// mutations only. Equal bytes do not establish an atomic camera/frame snapshot.
// Preserve NaN payloads and signed zero in the integer rows; JSON float views are
// convenience only. Never transpose, invert, normalize or repair these sources.
template <size_t Rows, size_t Columns>
Json FloatRowsField(Reader& read, uintptr_t view, uintptr_t offset)
{
    using Words = std::array<std::array<uint32_t, Columns>, Rows>;
    static_assert(sizeof(Words) == Rows * Columns * sizeof(uint32_t));
    Json result = { { "view_offset", offset }, { "byte_size", sizeof(Words) },
        { "layout", "consecutive_float32_rows" }, { "status", "unavailable" },
        { "repeated_source_words_equal", nullptr } };
    try
    {
        const auto words = read.Read<Words>(Address(view, offset));
        Json values = Json::array();
        bool allFinite = true;
        for (const auto& row : words)
        {
            Json valueRow = Json::array();
            for (const auto bits : row)
            {
                float value = 0;
                std::memcpy(&value, &bits, sizeof(value));
                allFinite &= std::isfinite(value);
                valueRow.push_back(std::isfinite(value) ? Json(value) : Json(nullptr));
            }
            values.push_back(std::move(valueRow));
        }
        result["source_uint32_rows"] = words;
        result["source_float_rows"] = std::move(values);
        result["all_finite"] = allFinite;
        result["status"] = "CPU_value_present";
        try
        {
            result["repeated_source_words_equal"] = words == read.Read<Words>(Address(view, offset));
        }
        catch (const std::exception& error) { result["repeat_read_failure"] = error.what(); }
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

Json PositionField(Reader& read, uintptr_t view)
{
    Json result = { { "view_offset", 0x70 }, { "byte_size", 12 },
        { "source_type", "int32_xyz_fixed_point" }, { "producer_scale_bits", 0x37000000u },
        { "status", "unavailable" } };
    try
    {
        const auto words = read.Read<std::array<uint32_t, 3>>(Address(view, 0x70));
        std::array<int32_t, 3> position {};
        std::memcpy(position.data(), words.data(), sizeof(position));
        std::array<float, 3> candidate {};
        for (size_t i = 0; i < candidate.size(); ++i)
            candidate[i] = static_cast<float>(position[i]) * (1.0f / 131072.0f);
        result["source_uint32_words"] = words;
        result["source_int32_xyz"] = position;
        result["producer_candidate"] = candidate; // Authored signed conversion, not inverse-view translation.
        result["status"] = "CPU_value_present";
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

template <typename T> Json UnsignedField(Reader& read, uintptr_t view, uintptr_t offset)
{
    static_assert(std::is_unsigned_v<T>);
    Json result = { { "view_offset", offset }, { "byte_size", sizeof(T) }, { "status", "unavailable" } };
    try
    {
        result["source_value"] = read.Read<T>(Address(view, offset));
        result["status"] = "CPU_value_present";
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

// Observed virtual getter RVA 0x18ec810 is exactly:
// 48 8d 41 10 c3 = lea rax,[rcx+0x10]; ret.
// ApplyDLSS 0x1d4fe38 calls this slot and reads returnedObject+0x1a0 at
// 0x1d4fe41. Reproduce only that bounded scalar source read, never the call.
Json ExplicitFrameIdSource(Reader& read, const GraphContext& context, uintptr_t image,
                           uintptr_t object, uintptr_t vtable, uintptr_t target)
{
    constexpr uintptr_t GetterRva = 0x18ec810, FrameIdObjectOffset = 0x1b0;
    constexpr std::array<uint8_t, 5> GetterBytes = { 0x48, 0x8d, 0x41, 0x10, 0xc3 };
    Json result = { { "status", "unavailable" }, { "getter_rva", GetterRva },
        { "source_object_offset", FrameIdObjectOffset }, { "byte_size", 4 },
        { "semantics", "CPU_source_for_later_explicit_Streamline_frame_ID" },
        { "virtual_call_performed", false }, { "value_passed_to_streamline", "not_observed" },
        { "frame_token", "not_observed" }, { "snapshot_atomic", false } };
    try
    {
        if (target != Address(image, GetterRva))
            throw std::runtime_error("frame-ID getter route not authenticated for scalar read");
        const auto code = read.Read<std::array<uint8_t, 5>>(target);
        if (code != GetterBytes)
            throw std::runtime_error("frame-ID getter bytes differ from authenticated leaf");
        const auto sourceAddress = Address(object, FrameIdObjectOffset);
        const auto value = read.Read<uint32_t>(sourceAddress);
        const bool unchanged = value == read.Read<uint32_t>(sourceAddress) &&
            object == read.Read<uintptr_t>(Address(context.context, 0)) &&
            vtable == read.Read<uintptr_t>(Address(object, 0)) &&
            target == read.Read<uintptr_t>(Address(vtable, 0x20)) &&
            code == read.Read<std::array<uint8_t, 5>>(target);
        result["repeated_source_fields_equal"] = unchanged;
        if (!unchanged)
            throw std::runtime_error("frame-ID source or getter route changed during reads");
        result["source_address"] = sourceAddress;
        result["source_value"] = value;
        result["status"] = "CPU_value_present";
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

// Observe any image-local virtual target, but expose an explicit frame-ID
// source only through the exact authenticated leaf above. No virtual call.
Json FrameIdVirtualRoute(Reader& read, const GraphContext& context, uintptr_t image)
{
    Json result = { { "status", "unavailable" }, { "context_object_offset", 0 },
        { "vtable_slot_offset", 0x20 }, { "virtual_call_performed", false },
        { "returned_object", "not_observed" }, { "frame_id", "not_observed" },
        { "target_executable", "not_established" }, { "snapshot_atomic", false } };
    try
    {
        const auto imageLast = Address(image, ImageBytes - 1);
        const auto object = read.Read<uintptr_t>(Address(context.context, 0));
        result["object_address"] = object;
        const auto vtable = read.Read<uintptr_t>(Address(object, 0));
        result["vtable_address"] = vtable;
        const auto target = read.Read<uintptr_t>(Address(vtable, 0x20));
        if (target < image || target > imageLast)
            throw std::runtime_error("frame-ID virtual target is outside authenticated image");
        result["target_address"] = target;
        result["target_rva"] = target - image;
        result["status"] = "image_local_target_observed";
        result["explicit_frame_id_source"] = ExplicitFrameIdSource(read, context, image, object, vtable, target);
    }
    catch (const std::exception& error) { result["reason"] = error.what(); }
    return result;
}

struct MotionScaleReader
{
    Reader& reader;
    bool Read(uintptr_t address, void* output, size_t bytes)
    {
        // The pure observer has only four small read shapes. Keep the shared
        // metadata budget and exact-read checks, with no native property calls.
        if (bytes == 4)
        {
            const auto value = reader.Read<uint32_t>(address);
            std::memcpy(output, &value, bytes);
        }
        else if (bytes == 5)
        {
            const auto value = reader.Read<std::array<char, 5>>(address);
            std::memcpy(output, value.data(), bytes);
        }
        else if (bytes == 11)
        {
            const auto value = reader.Read<std::array<char, 11>>(address);
            std::memcpy(output, value.data(), bytes);
        }
        else if (bytes == sizeof(std::array<uintptr_t, 3>))
        {
            const auto value = reader.Read<std::array<uintptr_t, 3>>(address);
            std::memcpy(output, value.data(), bytes);
        }
        else return false;
        return true;
    }
};

Json MotionScaleProvenance(Reader& read, uintptr_t image)
{
    Json result = { { "status", "unavailable" }, { "snapshot_atomic", false },
        { "semantics", "native_SL_normalized_motion_multiplier" },
        { "gpu_binding_proven", false }, { "defaults_used", false },
        { "section", "DLSS" }, { "names", { "MvecScaleX", "MvecScaleY" } },
        { "value_rvas", { 0x3464dc0, 0x3464e10 } } };
    MotionScaleReader host { read };
    FSRD::CyberpunkMotionScale::Snapshot observed;
    if (FSRD::CyberpunkMotionScale::Observe(host, image, observed))
    {
        result["source_bits"] = observed.words;
        result["status"] = "current_property_CPU_values";
        result["repeated_source_fields_equal"] = true;
    }
    else result["reason"] = "property identity, finite value or repeated snapshot refused";
    return result;
}

// The later authored producer (RVA 0x788a9c, publishing through 0x78933c)
// reads these view fields. We have NOT observed that producer or its frame token;
// these are candidates, not an early-ready replacement for final NGX constants.
Json CameraProvenance(Reader& read, const GraphContext& context, uintptr_t image)
{
    Json result = { { "schema", "optiscaler.fsr_rr.early_camera_sources.v2" },
        { "status", "current_view_CPU_sources_only" }, { "snapshot_atomic", false },
        { "streamline_producer_observed", false }, { "frame_token", "not_observed" },
        { "final_ngx_constants", "not_established" }, { "matrix_payload", "current_view_CPU_source_rows_only" },
        { "bound_shared_cb12_match", "not_validated" }, { "camera_position_binding", "not_observed" },
        { "jitter_free_projection", "not_captured_or_reconstructed" },
        { "previous_camera", "not_observed" }, { "effective_ngx_reset", "not_established" } };
    result["frame_id_virtual_route"] = FrameIdVirtualRoute(read, context, image);
    result["motion_scale"] = MotionScaleProvenance(read, image);
    result["near_plane"] = FloatField(read, context.view, 0xb0);
    result["far_plane"] = FloatField(read, context.view, 0xb4);
    // RVA 0x1e4338 multiplies this field by pi/180 before projection creation.
    // Cyberpunk copies degrees to SL despite that API's radians documentation.
    result["fov_degrees"] = FloatField(read, context.view, 0x90);
    result["aspect_ratio"] = FloatField(read, context.view, 0x98);
    result["projection_zoom"] = FloatField(read, context.view, 0x9c);
    result["authored_lens_offset"] = FloatRowsField<1, 2>(read, context.view, 0xa0);
    result["jitter_x"] = FloatField(read, context.view, 0x3e0);
    result["jitter_y"] = FloatField(read, context.view, 0x3e4, 0x80000000u);
    result["native_jitter_width"] = UnsignedField<uint32_t>(read, context.view, 0x3e8);
    result["native_jitter_height"] = UnsignedField<uint32_t>(read, context.view, 0x3ec);
    result["jitter_related_field"] = UnsignedField<uint32_t>(read, context.view, 0x3f0);
    result["projection_flags"] = UnsignedField<uint8_t>(read, context.view, 0x3f4);
    result["projection_flags"]["reverse_z_mask"] = 0x04;
    result["position"] = PositionField(read, context.view);
    result["basis_right"] = FloatRowsField<1, 3>(read, context.view, 0x2c0);
    result["basis_up"] = FloatRowsField<1, 3>(read, context.view, 0x2e0);
    result["basis_forward"] = FloatRowsField<1, 3>(read, context.view, 0x2d0);
    // Recompute RVA 0x1e412c and shared-CB producer 0x7854c0 establish these
    // CPU offsets. The cb12 ray matrix uses inverse_native_projection_jittered
    // times inverse_native_view with row 3 replaced by [0,0,0,1]. Capture sources
    // only: that product is an offline prediction, not the bound GPU payload.
    result["matrices"] = {
        { "native_view", FloatRowsField<4, 4>(read, context.view, 0xc0) },
        { "inverse_native_view", FloatRowsField<4, 4>(read, context.view, 0x180) },
        { "native_projection_jittered", FloatRowsField<4, 4>(read, context.view, 0x200) },
        { "inverse_native_projection_jittered", FloatRowsField<4, 4>(read, context.view, 0x1c0) },
        { "depth_converted_projection_jittered", FloatRowsField<4, 4>(read, context.view, 0x360) } };
    Json history = { { "view_offset", 0xef0 }, { "status", "unavailable" },
        { "semantics", "native_SL_reset_equals_zero" }, { "repeated_source_fields_equal", nullptr } };
    try
    {
        const auto value = read.Read<uint8_t>(Address(context.view, 0xef0));
        history["source_byte"] = value;
        history["producer_reset_candidate"] = value == 0;
        history["repeated_source_fields_equal"] = value == read.Read<uint8_t>(Address(context.view, 0xef0));
        history["status"] = "CPU_value_present";
    }
    catch (const std::exception& error) { history["reason"] = error.what(); }
    result["history"] = std::move(history);
    // A matching pointer is only a source identity check, not a frame token or
    // proof that the independently read view fields were unchanged in between.
    result["view_pointer_unchanged"] = nullptr;
    try
    {
        result["view_pointer_unchanged"] = context.view ==
            read.Read<uintptr_t>(Address(context.context, 0x18));
    }
    catch (const std::exception& error) { result["view_pointer_read_failure"] = error.what(); }
    return result;
}
}

std::string Describe(const void* context, uintptr_t authenticatedImageBase) noexcept
{
    try
    {
        Reader read;
        Json result = { { "schema", "optiscaler.fsr_rr.early_guide_availability.v1" },
            { "status", "CPU_metadata_only" }, { "image_authentication", "required_from_fog_probe_caller" },
            { "snapshot_atomic", false }, { "same_engine_frame", "not_established" },
            { "gpu_initialized", "not_established" }, { "resource_states", "not_observed" },
            { "shared_cb12", "not_captured" }, { "transparent_guide", "not_queried_before_its_producer" },
            { "retained_engine_pointers", false }, { "engine_helpers_called", false } };
        try
        {
            Address(authenticatedImageBase, ImageBytes - 1);
            const auto graph = ReadContext(read, reinterpret_cast<uintptr_t>(context));
            result["context"] = reinterpret_cast<uintptr_t>(context);
            result["view"] = graph.view;
            result["flags"] = graph.flags;
            result["namespace"] = graph.nameSpace;
            result["graph_selector"] = graph.selector;
            result["graph_counter"] = graph.counter;
            result["graph_position"] = graph.position;
            result["view_dimensions"] = { read.Read<uint32_t>(Address(graph.view, 0x34)),
                                          read.Read<uint32_t>(Address(graph.view, 0x38)) };
            const ViewFeatures features(read, graph);
            result["view_feature_words"] = features.Describe();
            Json inputs = Json::array();
            inputs.push_back(Resolve(read, graph, 0x63bcf380, "t0_gbuffer0"));
            inputs.push_back(Resolve(read, graph, 0x64bcf513, "t1_gbuffer1"));
            inputs.push_back(Resolve(read, graph, 0x65bcf6a6, "t2_gbuffer2"));
            inputs.push_back(Resolve(read, graph, 0x61f178d4, "t4_material_class_stencil_view"));
            const auto extra = ExtraSpecular(read, graph, features);
            inputs.push_back(extra);
            auto depth = Resolve(read, graph, 0xdebf0c27, "depth");
            depth["streamline_tag"] = 0;
            depth["authored_fog_binding"] = "pixel_srv_0";
            inputs.push_back(std::move(depth));
            inputs.push_back(Motion(read, graph, features));
            inputs.push_back(SpecularHitDistance(read, graph, features));
            for (auto& input : inputs)
                if (input.contains("handle"))
                    input["texture_registry"] = RegistryMapping(read, authenticatedImageBase,
                                                                 input["handle"].get<uint32_t>());
            result["inputs"] = std::move(inputs);
            result["camera_provenance"] = CameraProvenance(read, graph, authenticatedImageBase);
            Json settings = { { "NoV_mode", read.Read<int32_t>(Address(authenticatedImageBase, NoVModeRva)) },
                { "scope", "authored settings only; not a ready or complete early cb6 payload" } };
            if (!extra.contains("feature_0x35"))
                settings["extra_specular"] = "current view feature unavailable";
            else if (extra["feature_0x35"].get<bool>())
            {
                settings["extra_specular_enabled"] = read.Read<uint8_t>(Address(authenticatedImageBase, ExtraSpecularEnableRva));
                const auto bits = read.Read<uint32_t>(Address(authenticatedImageBase, ExtraSpecularScaleRva));
                float scale = 0;
                std::memcpy(&scale, &bits, sizeof(scale));
                settings["extra_specular_scale_bits"] = bits;
                settings["extra_specular_scale"] = std::isfinite(scale) ? Json(scale) : Json(nullptr);
            }
            else
            {
                settings["extra_specular_enabled"] = 0;
                settings["extra_specular_scale"] = 0;
                // The authored feature-disabled CB6 branch initializes this
                // float word to +0. Preserve raw-bit provenance in both branches.
                settings["extra_specular_scale_bits"] = uint32_t(0);
            }
            result["guide_settings"] = std::move(settings);
        }
        catch (const std::exception& error)
        {
            result["read_failure"] = error.what();
        }
        result["read_calls"] = read.calls;
        result["read_bytes"] = read.bytes;
        return result.dump();
    }
    catch (...)
    {
        return "{}"; // Never interrupt the original draw if metadata allocation fails.
    }
}
}
