#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <array>

// Research only. This helper does not hook, repeat, replace, or modify a game draw.
// Nothing is captured until Request() and Record() are explicitly called.
namespace FSRDFogLayerCapture
{
struct Texture
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    UINT subresource = 0;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct Layers
{
    Texture before;   // Private snapshot of the scene immediately before the authenticated fog draw.
    Texture after;    // Private snapshot immediately after that same original draw.
    Texture authored; // Unblended RGBA from a separately captured instance of that fog draw.
    // Optional, independently validated companion; never part of the three matching layers.
    // Exactly 5x1 RGBA32_UINT, texels = raw cb12 registers 21,22,23,24,27 in that order.
    Texture boundCb12;
    // Optional all-or-none private guide outputs, captured without format conversion:
    // diffuse RGBA8_UNORM, specular RGBA8_UNORM, normal/roughness RGBA16F.
    // Caller must establish their early dispatch/input provenance independently.
    std::array<Texture, 3> earlyGuides;
};

struct Status
{
    bool queued = false;
    bool busy = false;
    bool attempted = false;
    bool complete = false;
    std::string message;
    std::string directory;
    std::string kind; // "fog_layers" or "early_guides" when a request has been admitted.
};

// Initially one recorded attempt per process, including failed attempts. A queued
// request may be cancelled and requested again, but recorded work cannot be cancelled.
bool Request();
void CancelRequest();
bool WantsCapture();
Status GetStatus();

// Independent request KIND, sharing the same bounded one-attempt registry/worker.
// A fog request cannot consume/cancel a guide request, or vice versa. Only one
// recorded kind is allowed per process; no second 256 MiB batch is introduced.
bool RequestEarlyGuides();
void CancelEarlyGuideRequest();
bool WantsEarlyGuideCapture();

// Standalone native guide readback; no fog draw or scene snapshots are required.
// guides = u0 diffuse RGBA8_UNORM, u1 specular RGBA8_UNORM, u2 normal/roughness
// RGBA16F. All three must be distinct private immutable typed mip0/slice0,
// single-mip/single-array/single-sample textures of identical dimensions, in
// NON_PIXEL|PIXEL_SHADER_RESOURCE (0xc0), the private GuidePass's completed-recording
// state. No shader, exposure, normalization, alpha or integer conversion is made.
// Caller authenticates the original scene scope/inputs and restores any earlier
// compute binding changes. This helper only copies private textures/restores states.
// keepAlive is REQUIRED and must retain the dispatch Work's resources/PSO/heaps;
// earlier dispatch references must already be retained even if this call refuses.
// Copies are retained before recording on this exact list. Callback return is NOT
// submission/completion: the existing actual-list queue observer/fence and worker
// still determine GetStatus().complete. A false return can mean refused setup;
// true only means copies recorded (worker startup/disk failure is separate status).
// Output: <exe>/FSRRR-early-guide-captures/<UTC timestamp>/manifest.json plus three
// native companions. Caller provenance is evidence, not promoted to frame proof.
bool RecordEarlyGuides(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                       const std::array<Texture, 3>& guides, const std::string& provenanceJson,
                       const std::shared_ptr<void>& keepAlive) noexcept;

// Caller preconditions (not authenticated by this generic readback helper):
// - Authenticate the exact fog PSO, draw, bindings, subresource and blend descriptor.
// - Execute the original fog draw exactly once against the original scene target.
// - Any separate authored-layer draw must use the exact same shader/resources, with
//   blending disabled only on a private compatible target, no UAV/depth writes or
//   other side effects, and all original draw state restored afterward. Initialize
//   private targets where the draw does not cover every pixel.
// - Supply three DISTINCT private immutable snapshots, in their exact current states,
//   on this device/list. Never supply the game's original mutable render target.
// - Retain all resources referenced by any earlier caller-recorded commands even if
//   Record() fails. keepAlive can additionally retain the caller's PSO/heaps/resources
//   when this helper successfully records its copies; it is not a failure-path owner.
//
// Supports matching-extent, single-sample RGBA16F/RGBA32F subresources, with a 256 MiB
// total readback cap. before/after must use the same native view format; authored can
// use RGBA32F even when the main snapshots are RGBA16F. Capturing authored RGBA16F
// adds target quantization before any offline blend-equation check; prefer RGBA32F
// to avoid this additional loss. No shader/format/exposure/alpha transformation or
// inferred fog equation is performed by this helper.
// Optional boundCb12 must be a fourth distinct private immutable 5x1, mip0/slice0,
// single-sample typed RGBA32_UINT texture. It is copied as 80 native uint32 bytes
// into companions, not converted or admitted through the floating-layer checks.
// The shared 256 MiB readback budget includes this companion when present.
// Optional earlyGuides must be three distinct private mip0/slice0 textures,
// matching scene dimensions and the exact typed formats above. None may alias
// another layer/companion. Stored as separate native companions under the same
// completion fence; their pixels are not substituted into any scene layer.
// provenanceJson must be a JSON object; it is saved as caller-supplied evidence,
// not treated as proof that the above draw constraints were satisfied.
//
// Returns true when readback commands were recorded. CPU disk work and GPU completion
// checks run on a bounded background worker, never by waiting in Evaluate/Record.
// The existing FSRDSubmission queue observer must see this command list's submission.
// On abandoned work, failed signaling, or device removal, storage is conservatively
// retained rather than freed while GPU references may still exist. Check GetStatus()
// for asynchronous completion/failure. Output: <exe>/FSRRR-fog-captures/<UTC timestamp>/.
bool Record(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Layers& layers,
            const std::string& provenanceJson, const std::shared_ptr<void>& keepAlive = {});
} // namespace FSRDFogLayerCapture
