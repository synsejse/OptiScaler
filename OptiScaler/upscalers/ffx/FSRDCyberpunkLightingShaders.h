#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace FSRD::CyberpunkLightingShaders
{
struct Shader
{
    uint32_t selector;
    const char* name;
    size_t bytes;
    std::string_view sha256;
};
inline constexpr size_t VertexBytes = 2361;
inline constexpr std::string_view VertexSha256 =
    "174ce05e0a97ce65f80358b2a01bbadea4c314fb386cfac6064940a870f91a5a";
// Exact locally authenticated Cyberpunk 2.31 static shader-cache payloads.
// Only identities are shipped, never proprietary shader bytes.
inline constexpr std::array<Shader, 8> PixelShaders {{
    { 0xf7f22a61, "NoAmbient", 11867, "73b174b058493cda5b1483258025647b1a5597b019094c0aba001018b7d302b1" },
    { 0x90a79625, "NoEnvProbes", 14699, "7df07e2d0e594b5f65ab225198890d52c7ead1cced8a04bca9cc4eaa273649b9" },
    { 0xaeca39e1, "RayTracing_Reflection_NRD", 21107, "a93b4a970f12fbe78d587b61a2ad0a4ee6ea748799d59efaf65f413989532147" },
    { 0x77d76888, "RayTracing_Diffuse_NRD", 23811, "8fb0fab373d83bfceb46be5ac736fff53aee8e215c5ec9d941c55807b43934bb" },
    { 0xfd8c5631, "RayTracing_All_NRD", 21819, "036174ee2948960398ace78acfefbb2e0c2572a256e1bb5f6545091cb48811a5" },
    { 0xec8c6d60, "RayTracing_Reference", 12263, "02b8959f89a1196ebad6aa486be99f11dfb9af46d9451e0078f8e729900488cb" },
    { 0x2ef1e98f, "RayTracing_Reference_NRD", 12463, "051b199b86af56c7bae475bc150a7e5572b53daa44b117e473295100d9c38cc4" },
    { 0x57d00707, "LightIntegrate", 22919, "df2b6c86419a31c4a2755a4a9dc16be11afede7820b8c63ba64b100a5871fe46" }
}};

enum class CreationPath { Graphics, Stream };

class Registry
{
    struct Entry
    {
        Microsoft::WRL::ComPtr<IUnknown> identity;
        const Shader* shader = nullptr;
        CreationPath path = CreationPath::Graphics;
        bool conflict = false;
    };
    mutable std::mutex mutex_;
    std::array<Entry, 32> entries_ {};
    size_t count_ = 0, candidates_ = 0;

public:
    static constexpr size_t MaxPsos = 32, MaxCandidates = 128;

    // Invoke only after a successful ORIGINAL graphics/parsed-stream creation,
    // while the returned PSO and descriptor byte spans are valid API borrows.
    // hashExact(bytes,size) must snapshot/read the WHOLE span safely and return
    // its lowercase SHA256, or empty on failure. It runs outside our mutex.
    // The owning probe keeps this registry for its leaked diagnostic lifetime;
    // no eviction means retained COM identities cannot recycle into false hits.
    template<class HashExact>
    bool Record(ID3D12PipelineState* pso, const D3D12_SHADER_BYTECODE& vs,
                const D3D12_SHADER_BYTECODE& ps, CreationPath path, HashExact&& hashExact) noexcept
    {
        try
        {
            if (!pso || !vs.pShaderBytecode || !ps.pShaderBytecode || vs.BytecodeLength != VertexBytes ||
                (path != CreationPath::Graphics && path != CreationPath::Stream))
                return false;
            const Shader* matched = nullptr;
            for (const auto& candidate : PixelShaders)
                if (ps.BytecodeLength == candidate.bytes)
                    matched = &candidate;
            if (!matched) return false; // Never hash arbitrary-sized game shaders.
            {
                std::lock_guard lock(mutex_);
                if (candidates_ >= MaxCandidates) return false;
                ++candidates_;
            }
            if (hashExact(vs.pShaderBytecode, VertexBytes) != VertexSha256 ||
                hashExact(ps.pShaderBytecode, matched->bytes) != matched->sha256)
                return false;

            Microsoft::WRL::ComPtr<IUnknown> identity;
            if (FAILED(pso->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
                return false;
            // identity precedes this lock's lifetime: its Release always happens
            // after unlocking, including duplicate, capacity and exception exits.
            {
                std::lock_guard lock(mutex_);
                for (size_t i = 0; i < count_; ++i)
                    if (entries_[i].identity.Get() == identity.Get())
                    {
                        auto& entry = entries_[i];
                        entry.conflict |= entry.shader != matched;
                        return !entry.conflict;
                    }
                if (count_ >= MaxPsos) return false;
                auto& entry = entries_[count_];
                entry.shader = matched;
                entry.path = path;
                entry.identity.Swap(identity); // Empty slot; no COM call under lock.
                ++count_;
            }
            return true;
        }
        catch (...) { return false; } // Diagnostics must not break original creation.
    }

    // pso must again be a valid current API borrow, not a stale integer address.
    // A match identifies bytecode only: it does NOT prove current t37 contents,
    // descriptor binding, output normalization, or readiness for private GPU work.
    nlohmann::json Describe(ID3D12PipelineState* pso, uint32_t shaderArgument) const
    {
        using Json = nlohmann::json;
        Json result = { { "schema", "optiscaler.fsr_rr.lighting_shader_identity.v1" },
            { "shader_argument", shaderArgument }, { "matched", false },
            { "status", "pso_not_observed" }, { "pso", uintptr_t(pso) },
            { "shader_bytes_retained", false }, { "exposure_binding", "not_observed" } };
        Microsoft::WRL::ComPtr<IUnknown> identity;
        if (!pso || FAILED(pso->QueryInterface(IID_PPV_ARGS(&identity))) || !identity)
        {
            result["status"] = "pso_identity_unavailable";
            return result;
        }
        result["pso_identity"] = uintptr_t(identity.Get());
        {
            std::lock_guard lock(mutex_);
            result["retained_pso_count"] = count_;
            result["candidate_budget_exhausted"] = candidates_ >= MaxCandidates;
            result["pso_capacity_reached"] = count_ >= MaxPsos;
            for (size_t i = 0; i < count_; ++i)
                if (entries_[i].identity.Get() == identity.Get())
                {
                    const auto& entry = entries_[i];
                    result["variant"] = entry.shader->name;
                    result["expected_shader_argument"] = entry.shader->selector;
                    result["vs_bytes"] = VertexBytes;
                    result["vs_sha256"] = VertexSha256;
                    result["ps_bytes"] = entry.shader->bytes;
                    result["ps_sha256"] = entry.shader->sha256;
                    result["creation_path"] = entry.path == CreationPath::Graphics ?
                        "CreateGraphicsPipelineState" : "CreatePipelineState";
                    result["status"] = entry.conflict ? "conflicting_creation_observations" :
                        shaderArgument == entry.shader->selector ? "matched" : "selector_mismatch";
                    result["matched"] = !entry.conflict && shaderArgument == entry.shader->selector;
                    break;
                }
        }
        return result;
    }
};
} // namespace FSRD::CyberpunkLightingShaders
