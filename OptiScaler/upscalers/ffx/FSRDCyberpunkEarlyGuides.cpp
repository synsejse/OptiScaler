#include "pch.h"
#include "FSRDCyberpunkEarlyGuides.h"

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

Json ExtraSpecular(Reader& read, const GraphContext& context)
{
    Json result = { { "input", "t5_extra_specular" }, { "status", "unavailable" },
                    { "gpu_initialized", "not_established" } };
    try
    {
        const bool enabled = !(context.flags & 1) &&
            ((read.Read<uint64_t>(Address(context.view, 0x17d0)) >> 0x35) & 1);
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
            Json inputs = Json::array();
            inputs.push_back(Resolve(read, graph, 0x63bcf380, "t0_gbuffer0"));
            inputs.push_back(Resolve(read, graph, 0x64bcf513, "t1_gbuffer1"));
            inputs.push_back(Resolve(read, graph, 0x65bcf6a6, "t2_gbuffer2"));
            inputs.push_back(Resolve(read, graph, 0x61f178d4, "t4_material_uint2"));
            const auto extra = ExtraSpecular(read, graph);
            inputs.push_back(extra);
            result["inputs"] = std::move(inputs);
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
