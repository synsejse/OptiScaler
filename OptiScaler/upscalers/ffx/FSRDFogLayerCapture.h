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
    // Optional private raw hardware-depth snapshot from this original Fog scope.
    // Exact typed R32_FLOAT, mip0/slice0, single-mip/array/sample, scene extent,
    // state0xc0. This is not a linearized distance or the material stencil plane.
    Texture hardwareDepth;
    // Optional all-or-none private independent RESET outputs. In order:
    // converted radiance, AMD denoised radiance, production composed color.
    // Exactly typed RGBA16_FLOAT, mip0/slice0, one mip/array/sample, scene extent,
    // exact state0xc0. These are diagnostic copies, never scene replacements.
    std::array<Texture, 3> privateReset;
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

// One recorded attempt of EACH kind per process, including failed attempts. A
// queued request may be cancelled/requeued; recorded work cannot be cancelled or
// retried. Each batch keeps the 256 MiB readback cap (512 MiB for both fixed slots).
// The slots do not establish a pair/frame relation; caller provenance must do that.
bool Request();
void CancelRequest();
bool WantsCapture();
Status GetStatus(); // Fog only, never the latest/other kind's status.

// Independent early-guide slot. Both kinds may be queued/recorded/completed in
// either order; neither consumes, cancels, retries or retires the other's work.
bool RequestEarlyGuides();
void CancelEarlyGuideRequest();
bool WantsEarlyGuideCapture();
Status GetEarlyGuideStatus();

// Standalone native guide readback; no fog draw or scene snapshots are required.
// guides = u0 diffuse RGBA8_UNORM, u1 specular RGBA8_UNORM, u2 normal/roughness
// RGBA16F. All three must be distinct private immutable typed mip0/slice0,
// single-mip/single-array/single-sample textures of identical dimensions, in
// NON_PIXEL|PIXEL_SHADER_RESOURCE (0xc0), the private GuidePass's completed-recording
// state. No shader, exposure, normalization, alpha or integer conversion is made.
// Caller authenticates the original scene scope/inputs and restores any earlier
// compute binding changes. This helper only records native copies/restores states.
// keepAlive is REQUIRED and must retain the dispatch Work's resources/PSO/heaps;
// earlier dispatch references must already be retained even if this call refuses.
// Copies are retained before recording on this exact list. Callback return is NOT
// submission/completion: the existing actual-list queue observer/fence and worker
// still determine GetEarlyGuideStatus().complete. A false return can mean refused setup;
// true only means copies recorded (worker startup/disk failure is separate status).
// Optional exposureWords is a fourth distinct private immutable 2x1 RGBA32_UINT
// mip0/slice0, single-mip/array/sample texture in the same 0xc0 state. Row-major
// RGBA words0..6 are the exact source uint32 bits at byte offsets0,4,...,24;
// word7 must be zero padding, checked only after the GPU completion fence. The
// helper does not read an engine exposure resource or interpret/normalize words.
// Caller must supply actual GPU-word producer/binding provenance separately and
// retain its producer Work in keepAlive; a well-formed texture is NOT that proof.
// Optional lightingT8 is a distinct caller-owned typed RGBA16_FLOAT mip0/slice0,
// single-mip/array/sample texture matching the guide render extent. Pass the
// required read-state tag0x8c0 (PIXEL|NON_PIXEL_SHADER_RESOURCE|COPY_SOURCE).
// It may be a private snapshot OR the actual original-lighting pixel t8, owned
// at its current valid use, with no writable alias. Caller must prove the engine
// requested and flushed ALL required read bits on THIS recording; the actual
// native state may be a read-only superset, not an established exact mask. The
// helper immediately records a native readback copy with NO source barriers;
// it neither guesses StateBefore nor changes the engine-managed read state.
// The worker reads only the copied readback memory, never later source pixels.
// Caller must retain earlier references even
// if this call refuses. The helper preserves encoded RGBA bits: this input is
// NOT established as raw-ray or undenoised radiance, and
// no exposure, alpha, demodulation or other shader math is performed here.
// Optional rayCopies is an all-or-none pair of DISTINCT private immutable snapshots:
// [0] motion RGBA16_FLOAT, [1] hit R32_FLOAT. Both must be typed mip0/slice0,
// single-mip/array/sample, match the active guide extent and be in exact state0xc0.
// Neither may alias any guide, exposure or lighting companion. The caller retains
// their original-ray copy Work in keepAlive and independently proves the original
// ray snapshot stage, motion/hit units, selected writer and any final-write claim;
// layout acceptance and a completed readback do NOT establish those semantics.
// All channels/bits are copied unchanged, with no unpacking or hit normalization.
// All entries share the existing256MiB cap, owner and completion fence.
// Output: <exe>/FSRRR-early-guide-captures/<UTC timestamp>/manifest.json plus three
// guides and optional exposure_words.rgba32u / lighting_t8.rgba16f companions,
// ray_motion.rgba16f and ray_hit.r32f. A non-null retention ticket is required
// before the first readback command, including when no companions are supplied.
// Caller evidence is not frame proof. Existing three/four-entry calls are unchanged.
bool RecordEarlyGuides(ID3D12Device* device, ID3D12GraphicsCommandList* list,
                       const std::array<Texture, 3>& guides, const std::string& provenanceJson,
                       const std::shared_ptr<void>& keepAlive, const Texture* exposureWords = nullptr,
                       const Texture* lightingT8 = nullptr,
                       const std::array<Texture, 2>* rayCopies = nullptr) noexcept;

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
// Optional hardwareDepth is a distinct PRIVATE typed R32_FLOAT snapshot from the
// current Fog depth plane, matching scene extent, mip0/slice0, one mip/array/sample,
// in exact state0xc0. It must not alias any layer/companion, including through a
// different COM interface. keepAlive is required when supplied and must retain
// its producer's recording owners; earlier references must already be retained.
// Stored as hardware_depth.r32f with unchanged native R float32 bits. The caller
// proves original graph/plane/binding, snapshot order and hardware-depth encoding;
// no linearization, camera reconstruction, depth convention or same-frame relation
// to a SEPARATE capture is inferred here. Shares the existing budget/fence/worker.
// Optional privateReset requires all three private immutable RGBA16_FLOAT textures
// described above, or none. Each must have a distinct canonical resource identity
// from every scene layer and companion (including other private RESET outputs).
// keepAlive is required and must retain the recording owners; it MUST NOT own the
// submission ticket retaining this batch (no ownership cycle). Earlier GPU references
// remain the caller's responsibility even if this helper refuses before retention.
// Native files private_reset_{radiance,denoised,composed}.rgba16f share the existing
// 256MiB budget and actual-list submission fence. No signal/alpha/exposure transform
// is performed; provider input semantics, current-frame joins and RESET context
// admission are caller evidence, not inferred from the three file layouts. These
// companions do NOT enable a live correction or modify any original scene layer.
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
