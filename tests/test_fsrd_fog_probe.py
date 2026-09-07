"""Fog probe/capture integration guards; Windows CI/live testing are separate."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp").read_text()


def function(name):
    start = SOURCE.index(name + "(")
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[opening:end]


class FogProbe(unittest.TestCase):
    def test_sl_token_is_only_nested_call_metadata_not_fog_frame_proof(self):
        observer = function("ObserveNgxInput")
        self.assertLess(observer.index("if (!endpointActive.load"),
                        observer.index("SlEvaluationProvenance::Current()"))
        self.assertEqual(observer.count("SlEvaluationProvenance::Current()"), 1)
        self.assertEqual(observer.count('{ "sl_evaluation_scope", slProvenance }'), 2)
        for field in ('"optiscaler.fsr_rr.sl_evaluation_scope.v1"',
                      '"command_buffer_canonical_identity", "not_established"',
                      '"fog_frame_association", "not_established"',
                      '"raw_command_buffer_equals_ngx_list"'):
            self.assertIn(field, observer)
        self.assertIn('slEvaluation.observed ? Json(slEvaluation.frameIndex) : Json(nullptr)', observer)
        # Token metadata must not turn a candidate into an accepted frame.
        admission = observer.split("if (trace->candidates.size()", 1)[1].split("{", 1)[0]
        self.assertNotIn("slEvaluation", admission)

    def test_opt_in_precedes_initialization_and_hooks_need_authentication(self):
        init = function("Initialize")
        self.assertLess(init.index("if (!enabled)"), init.index("std::call_once"))
        self.assertLess(init.index("if (!Authenticate(entry))"), init.index("DetourAttach"))
        self.assertIn("if (!active.load() || !device)", function("HookDevice"))
        self.assertIn("if (!active.load() || !list)", function("HookCommandList"))

    def test_complete_executable_and_live_entry_authentication(self):
        authenticate = function("Authenticate")
        for evidence in ("Cyberpunk2077.exe", "version_t(3, 0, 80, 51928)", "version_t(2, 3, 1, 0)",
                         "hash.Finish() != ExeSha256", "ReadFile(", "IMAGE_FILE_MACHINE_AMD64",
                         "0x68af45ea", "0x04efc000", "prologue != FogPrologue", "MEM_IMAGE", "PAGE_GUARD"):
            self.assertIn(evidence, authenticate)
        self.assertIn("a7de82945c03e041fc7339fcf9066224d98db2f5d80fea50f7947bb350a60991", SOURCE)
        prologue = SOURCE.split("FogPrologue = {", 1)[1].split("};", 1)[0]
        self.assertEqual(bytes(int(x, 16) for x in re.findall(r"0x[0-9a-f]+", prologue)),
                         bytes.fromhex("48 8b c4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8d 6c 24 a0 48 81 ec 60 01 00 00"))

    def test_original_entry_points_called_exactly_once(self):
        for hook, original in (("HookFogNode", "originalFogNode"), ("HookSetPso", "originalSetPso"),
                               ("HookSetRtv", "originalSetRtv"), ("HookDraw", "originalDraw"),
                               ("HookDrawIndexed", "originalDrawIndexed"),
                               ("HookCreateGraphics", "originalCreateGraphics"),
                               ("HookCreateStream", "originalCreateStream")):
            self.assertEqual(function(hook).count(original + "("), 1)

    def test_thread_local_scope_restores_parent_and_does_not_escape(self):
        self.assertIn("thread_local Scope* scope = nullptr", SOURCE)
        hook = function("HookFogNode")
        self.assertLess(hook.index("scope = &current"), hook.index("originalFogNode(node, context)"))
        self.assertIn("~RestoreScope() { scope = previous; }", hook)
        self.assertIn("if (!scope || inMetadata)", function("LogDraw"))

    def test_metadata_only_path_cannot_capture_or_mutate_render_state(self):
        draw = function("HookDraw")
        self.assertLess(draw.index("captureEnabled.load() && !inMetadata && scope"), draw.index("PrepareCapture("))
        self.assertLess(function("PrepareCapture").index("!captureEnabled.load()"),
                        function("PrepareCapture").index("CopyMain("))
        self.assertLess(function("PrepareAuthoredPso").index("!captureEnabled.load()"),
                        function("PrepareAuthoredPso").index("originalCreateGraphics("))
        for name in ("HookFogNode", "HookSetPso", "HookSetRtv", "LogDraw"):
            for mutation in ("->Draw", "->Dispatch", "->Copy", "->ResourceBarrier", "->SetPipelineState",
                             "->OMSetRenderTargets", "WriteFile(", "CreateRenderTargetView"):
                self.assertNotIn(mutation, function(name))
        self.assertIn("originalSetRtv(list, count, rtvs, contiguous, dsv)", SOURCE)

    def test_never_claims_preexisting_pso_or_rtv_was_observed(self):
        draw = function("LogDraw")
        self.assertIn("s.psoList == list && s.pso", draw)
        self.assertIn("s.rtvList == list", draw)
        self.assertIn('"pso_observed_in_scope"', draw)
        self.assertIn('"rtv_observed_in_scope"', draw)
        self.assertIn('"unknown/not authenticated"', draw)
        self.assertIn("MaxDrawLogs = 32", SOURCE)
        self.assertIn("MaxPsoLogs = 16", SOURCE)

    def test_pso_authentication_uses_full_hash_and_fails_closed(self):
        # Lighting identities have their own bounded registry/hash tests. These
        # assertions concern the unchanged fog-specific admission loop.
        pso = function("RecordPso").split("for (const auto& identity : FogShaders)", 1)[1]
        self.assertLess(pso.index("desc.PS.BytecodeLength != identity.bytes"), pso.index("hash.Add("))
        self.assertLess(pso.index("hash.Finish() != identity.sha256"), pso.index("data.tagged.push_back"))
        for digest in ("a7a57220b8f5c1abddd8205d626ece403df647152d9a7afe147d5c285bc6589a",
                       "79fc7accdf41dd02a41d101effc20786fb809c867bc31502abc7c51548130f78",
                       "3d2bfd8ac57ac8673accb3fa48757e1848481b0a3ef5ade3feba6153095ccf6b",
                       "0953f807c7a784c8012fe37eb46bd3ea40ecb17aa94baba36e10ad61d1206738"):
            self.assertIn(digest, SOURCE)
        self.assertIn("FAILED(D3DX12ParsePipelineStream(*desc, &parsed))", function("HookCreateStream"))
        self.assertIn("pipeline-library loads not authenticated", SOURCE)

    def test_blend_and_target_metadata_use_only_effective_descriptors(self):
        pso = function("RecordPso")
        self.assertIn("std::min(desc.NumRenderTargets", pso)
        self.assertIn("desc.BlendState.IndependentBlendEnable ? i : 0", pso)
        for field in ("src", "dst", "op", "src_alpha", "dst_alpha", "op_alpha", "write_mask",
                      "format", "root_signature", "sample_count"):
            self.assertIn('"' + field + '"', pso)

    def test_rearm_is_explicit_bounded_and_preserves_sequence(self):
        poll = function("PollRearm")
        self.assertIn("MaxRearms = 2", SOURCE)
        self.assertIn("now + 1000", poll)
        self.assertIn("arm.load() >= MaxRearms", poll)
        self.assertIn('Util::ExePath().parent_path() / L"FSRRR-fog-probe.request"', poll)
        self.assertIn("FILE_ATTRIBUTE_DIRECTORY", poll)
        self.assertLess(poll.index("if (!DeleteFileW(request.c_str()))"), poll.index("drawLogs.store(0)"))
        self.assertNotIn("scopes.store", poll)
        self.assertIn("not a frame association", poll)

    def test_capture_requires_separate_flag_and_explicit_one_shot_request(self):
        self.assertIn("FfxDenoiserCyberpunkFogCapture.value_or_default()", function("Initialize"))
        poll = function("PollRearm")
        self.assertIn("captureEnabled.load() && captureTrackingValid.load()", poll)
        self.assertIn('L"FSRRR-fog-capture.request"', poll)
        self.assertIn("FSRDFogLayerCapture::Request()", poll)
        prepare = function("PrepareCapture")
        self.assertIn("!FSRDFogLayerCapture::WantsCapture()", prepare)
        self.assertIn("captureStarted.exchange(true)", prepare)

    def test_rtv_resource_is_acquired_at_bind_never_from_late_cpu_handle(self):
        bind = function("HookSetRtv")
        self.assertIn("FSRDFogLayerCapture::WantsCapture()", bind)
        self.assertIn("bound.resource = slot->resource", bind)
        self.assertIn("bound.view = slot->view", bind)
        self.assertIn("bound.slotGeneration = slot->generation", bind)
        self.assertLess(bind.index("bound.resource = slot->resource"), bind.index("originalSetRtv("))
        prepare = function("PrepareCapture")
        self.assertIn("const auto& bound = s.boundRtv", prepare)
        self.assertIn("plan->main = bound.resource", prepare)
        self.assertNotIn("FindRtv(", prepare)

    def test_frozen_original_rtv_restores_binding_despite_cpu_slot_recycling(self):
        prepare = function("PrepareCapture")
        self.assertIn("heapDesc.NumDescriptors = 2", prepare)
        self.assertIn("CreateRenderTargetView(plan->main.Get(), &plan->originalView, plan->frozenOriginalRtv)", prepare)
        finish = function("FinishCapture")
        self.assertIn("originalSetRtv(list, 1, &plan.frozenOriginalRtv, plan.contiguous, nullptr)", finish)
        self.assertNotIn("&plan.originalRtv", finish)
        self.assertIn("s.hasDsv", prepare)

    def test_state_history_is_reset_scoped_and_ambiguity_refuses_capture(self):
        reset = function("HookReset")
        self.assertLess(reset.index("originalReset("), reset.index("if (SUCCEEDED(hr))"))
        self.assertIn("state = {}", reset)
        self.assertIn("state.generation = ++data.nextRecording", reset)
        self.assertIn("data.lists.erase(identity.Get())", function("HookCreateList"))
        self.assertIn("state.known = false", function("HookClearState"))
        self.assertIn("state.known = false", function("HookExecuteBundle"))
        prepare = function("PrepareCapture")
        for guard in ("!foundState->second.known", "foundState->second.predicated",
                      "foundState->second.renderPass", "foundState->second.queryCount",
                      "foundState->second.viewportCount != 1", "foundState->second.scissorCount != 1"):
            self.assertIn(guard, prepare)
        self.assertIn("D3D12_QUERY_TYPE_TIMESTAMP", function("HookEndQuery"))
        self.assertIn("QueryInterface(IID_PPV_ARGS(&identity))", function("ListIdentity"))

    def test_exact_vertex_and_pixel_code_and_no_side_effect_pipeline_guards(self):
        clone = function("PrepareAuthoredPso")
        self.assertIn("174ce05e0a97ce65f80358b2a01bbadea4c314fb386cfac6064940a870f91a5a", SOURCE)
        self.assertIn("hash.Finish() != FogVertexSha256", clone)
        for guard in ("desc.GS.BytecodeLength", "desc.HS.BytecodeLength", "desc.DS.BytecodeLength",
                      "desc.StreamOutput.NumEntries", "desc.StreamOutput.NumStrides", "desc.InputLayout.NumElements",
                      "desc.DepthStencilState.DepthEnable", "desc.DepthStencilState.StencilEnable",
                      "desc.SampleDesc.Count != 1", "desc.NumRenderTargets != 1", "!streamSafe"):
            self.assertIn(guard, clone)
        self.assertIn("tagged.vertexBytes.assign", clone)
        self.assertIn("tagged.pixelBytes.assign", clone)
        self.assertIn("tagged.root = desc.pRootSignature", clone)
        self.assertIn("clone.CachedPSO = {}", clone)
        self.assertIn("DXGI_FORMAT_R32G32B32A32_FLOAT", clone)
        self.assertIn("BlendEnable = FALSE", clone)

    def test_capture_storage_retained_before_first_gpu_copy(self):
        prepare = function("PrepareCapture")
        self.assertLess(prepare.index("FSRDSubmission::Retain("), prepare.index("CopyMain("))
        self.assertIn("mainBytes + copyCount * copyBytes + authoredBytes > MaxCaptureTextureBytes", prepare)
        self.assertIn("MaxCaptureTextureBytes = 256ull * 1024 * 1024", SOURCE)
        self.assertIn("D3D12_RTV_DIMENSION_TEXTURE2D", prepare)
        self.assertIn("bound.view.Texture2D.MipSlice != 0", prepare)
        self.assertIn("desc.DepthOrArraySize != 1", prepare)
        self.assertIn("desc.SampleDesc.Count != 1", prepare)
        self.assertIn("plan->layers", function("FinishCapture"))
        self.assertNotIn("Record(plan->device.Get(), list, plan->main", SOURCE)

    def test_same_draw_before_after_then_private_layer_zero_clear_and_restore(self):
        draw = function("HookDraw")
        self.assertLess(draw.index("PrepareCapture("), draw.index("originalDraw("))
        self.assertLess(draw.index("originalDraw("), draw.index("FinishCapture("))
        finish = function("FinishCapture")
        self.assertLess(finish.index("CopyMain("), finish.index("ClearRenderTargetView("))
        self.assertIn("const FLOAT clear[] = { 0, 0, 0, 0 }", finish)
        self.assertLess(finish.index("&plan->authoredRtv"), finish.index("originalDraw(list, 3, 1, 0, 0)"))
        self.assertIn("~RestoreDrawState()", finish)
        self.assertLess(finish.index("originalDraw(list, 3, 1, 0, 0)"), finish.index("FSRDFogLayerCapture::Record("))
        for forbidden in ("->SetGraphicsRootSignature", "->SetDescriptorHeaps", "->RSSetViewports", "->RSSetScissorRects"):
            self.assertNotIn(forbidden, SOURCE)
        self.assertIn("~RestoreMainState()", function("CopyMain"))

    def test_capture_does_not_claim_a_later_rr_frame_association(self):
        prepare = function("PrepareCapture")
        self.assertIn('{ "rr_frame_association", "not_established" }', prepare)
        self.assertIn('"scope_serial"', prepare)
        self.assertIn('"command_list_generation"', prepare)
        self.assertIn('"viewports"', prepare)
        self.assertIn('"scissor_rects"', prepare)

    def test_descriptor_and_list_tracking_bounded_and_forwarded_once(self):
        self.assertIn("MaxRtvHeaps = 512, MaxRtvSlots = 65536, MaxCommandLists = 2048", SOURCE)
        self.assertIn("captureTrackingValid.store(false)", function("Track"))
        copied = function("TrackDescriptorCopy")
        self.assertLess(copied.index("snapshot.push_back"), copied.index("*slot = snapshot[cursor]"))
        for hook, original in (("HookCreateHeap", "originalCreateHeap"), ("HookCreateRtv", "originalCreateRtv"),
                               ("HookCreateList", "originalCreateList"), ("HookCopyDescriptors", "originalCopyDescriptors"),
                               ("HookCopyDescriptorsSimple", "originalCopyDescriptorsSimple"), ("HookReset", "originalReset"),
                               ("HookClearState", "originalClearState"), ("HookSetPredication", "originalSetPredication"),
                               ("HookBeginQuery", "originalBeginQuery"), ("HookEndQuery", "originalEndQuery"),
                               ("HookExecuteBundle", "originalExecuteBundle"), ("HookSetViewports", "originalSetViewports"),
                               ("HookSetScissors", "originalSetScissors"), ("HookBeginRenderPass", "originalBeginRenderPass"),
                               ("HookEndRenderPass", "originalEndRenderPass")):
            self.assertEqual(function(hook).count(original + "("), 1)

    def test_list_registry_evicts_oldest_reset_without_inventing_state_or_disabling_tracking(self):
        reset = function("HookReset")
        self.assertIn("!data.lists.contains(identity.Get()) && data.lists.size() >= MaxCommandLists", reset)
        self.assertIn("a.second.generation < b.second.generation", reset)
        self.assertLess(reset.index("data.lists.erase(oldest)"), reset.index("data.lists[identity.Get()]"))
        self.assertIn("state = {}", reset)
        self.assertIn("state.generation = ++data.nextRecording", reset)
        self.assertNotIn("captureTrackingValid.store(false)", reset)
        self.assertNotIn("command-list provenance budget exhausted", reset)
        track = function("TrackList")
        self.assertIn("data.lists.find(identity.Get())", track)
        self.assertNotIn("data.lists[", track)
        self.assertIn("foundState == data.lists.end()", function("PrepareCapture"))
        self.assertIn("found != data.lists.end()", function("ObserveNgxInput"))

    def test_list_registry_size_and_eviction_logging_are_bounded(self):
        reset = function("HookReset")
        self.assertIn("MaxListEvictionLogs = 8", SOURCE)
        self.assertIn("data.listEvictionLogs < MaxListEvictionLogs", reset)
        self.assertIn("++data.listEvictions", reset)
        self.assertIn("data.lists.size() > data.peakListCount", reset)
        for count in (1, 128, 512, 1024):
            self.assertIn("count == " + str(count), reset)
        self.assertIn("count == MaxCommandLists", reset)
        for field in ("retained={}/{}", "total_evictions={}", "evicted_Reset_generation={}",
                      "incoming_Reset_generation={}", "state_payload_bytes={}"):
            self.assertIn(field, reset)

    def test_rtv_heap_budget_reports_exact_cause_counts_and_bounded_milestones(self):
        create = function("HookCreateHeap")
        self.assertIn("data.heaps.size() >= MaxRtvHeaps", create)
        self.assertIn("RTV provenance budget exhausted: reason={}", create)
        for field in ("retained_heaps={}/{}", "retained_slots={}/{}", "requested_heaps=1",
                      "requested_heap_slots={}", "reserved_slots={}", "requested_metadata_slots=0",
                      "slot_payload_bytes={}"):
            self.assertIn(field, create)
        for reason in ("zero-sized heap", "heap limit"):
            self.assertIn('"' + reason + '"', create)
        for count in (1, 64, 128, 256, 512):
            self.assertIn("heapCount == " + str(count), create)

    def test_sparse_rtv_storage_bounds_written_entries_not_reserved_heap_space(self):
        self.assertIn("std::unordered_map<UINT, RtvSlot> slots", SOURCE)
        create = function("HookCreateHeap")
        self.assertIn("record.descriptorCount = desc->NumDescriptors", create)
        self.assertNotIn("slots.resize", create)
        self.assertNotIn("desc->NumDescriptors > MaxRtvSlots", create)
        self.assertNotIn("data.slots += desc->NumDescriptors", create)
        lookup = function("FindRtv")
        self.assertIn("slot >= heap.descriptorCount", lookup)
        self.assertIn("heap.slots.find(UINT(slot))", lookup)
        self.assertLess(lookup.index("if (!createWrittenSlot)"), lookup.index("heap.slots.try_emplace"))
        self.assertLess(lookup.index("if (data.slots >= MaxRtvSlots)"), lookup.index("heap.slots.try_emplace"))
        self.assertIn("if (inserted) ++data.slots", lookup)
        self.assertIn("requested_heaps=0 requested_slots=1", lookup)

    def test_only_rtv_writes_allocate_sparse_entries_and_unknown_copy_invalidates(self):
        self.assertIn("FindRtv(data, destination.ptr, nullptr, nullptr, true)", function("HookCreateRtv"))
        copy = function("TrackDescriptorCopy")
        self.assertIn("FindRtv(data, sources[range].ptr + SIZE_T(i) * increment)", copy)
        self.assertIn("FindRtv(data, destinations[range].ptr + SIZE_T(i) * increment, nullptr, nullptr, true)", copy)
        self.assertIn("snapshot.push_back(slot ? *slot : RtvSlot {})", copy)
        self.assertIn("*slot = snapshot[cursor]", copy)
        self.assertIn("FindRtv(data, rtvs[0].ptr, &heap, &index)", function("HookSetRtv"))

    def test_default_rtv_is_only_documented_typed_single_slice_single_sample_rgba16f(self):
        create = function("HookCreateRtv")
        default = create.split("else if (resource)", 1)[1]
        for requirement in ("resourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT",
                            "resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D",
                            "resourceDesc.DepthOrArraySize == 1", "resourceDesc.SampleDesc.Count == 1"):
            self.assertIn(requirement, default)
        self.assertIn("slot->view.Texture2D.MipSlice = 0", default)
        self.assertIn("slot->view.Texture2D.PlaneSlice = 0", default)
        self.assertIn("slot->documentedDefault = true", default)
        self.assertIn("Typeless, array, MSAA and other default views remain unknown", default)
        self.assertIn("bound.documentedDefault = slot->documentedDefault", function("HookSetRtv"))
        self.assertIn('"descriptor_source"', function("PrepareCapture"))

    def test_endpoint_observer_inactive_path_has_no_com_or_gpu_work(self):
        observe = function("ObserveNgxInput")
        self.assertLess(observe.index("if (!endpointActive.load"), observe.index("Metadata("))
        self.assertLess(observe.index("return {};"), observe.index("EndpointListIdentity("))
        for mutation in ("->Draw", "->Dispatch", "->Copy", "->ResourceBarrier", "->SetPipelineState",
                         "->OMSetRenderTargets", "FSRDResearch::Request", "FSRDSubmission::Retain"):
            self.assertNotIn(mutation, observe)
        self.assertIn("noexcept", SOURCE[SOURCE.index("CaptureCandidate ObserveNgxInput"):].split("{", 1)[0])

    def test_endpoint_origin_is_published_only_after_original_capture_draw(self):
        draw = function("HookDraw")
        self.assertLess(draw.index("originalDraw("), draw.index("PublishFogEndpoint(plan)"))
        self.assertLess(draw.index("if (plan)"), draw.index("PublishFogEndpoint(plan)"))
        publish = function("PublishFogEndpoint")
        self.assertIn("data.endpoint = plan->endpoint", publish)
        self.assertIn("endpointActive.store(true", publish)
        self.assertEqual(SOURCE.count("endpointActive.store(true"), 1)
        prepare = function("PrepareCapture")
        self.assertLess(prepare.index("!FSRDFogLayerCapture::WantsCapture()"), prepare.index("plan->endpoint ="))
        self.assertLess(prepare.index("OwnEndpointResource("), prepare.index("CopyMain("))
        self.assertIn('plan->provenance["endpoint_origin"]', prepare)

    def test_endpoint_recording_identity_handles_wrapper_without_submission_hooks(self):
        identity = function("EndpointListIdentity")
        self.assertIn("0xadec44e2, 0x61f0, 0x45c3", identity)
        self.assertIn("0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff", identity)
        self.assertIn("object->QueryInterface(IID_PPV_ARGS(&identity))", identity)
        self.assertNotIn("HookToQueue(", identity)
        self.assertNotIn("PrepareSubmission(", identity)
        observe = function("ObserveNgxInput")
        self.assertIn("generation == trace->generation", observe)
        self.assertIn("identity.Get() == trace->list.Get()", observe)
        self.assertIn("++found->second.endpointOrdinal", observe)
        self.assertIn("captureTrackingValid.load() && found != data.lists.end()", observe)
        self.assertIn('"different_command_list"', observe)
        self.assertIn('"different_Reset_recording"', observe)

    def test_endpoint_identity_owners_and_logs_are_bounded(self):
        self.assertIn("MaxNgxEndpoints = 8", SOURCE)
        self.assertIn("MaxEndpointResourceBytes = 256ull * 1024 * 1024", SOURCE)
        own = function("OwnEndpointResource")
        self.assertIn("resource->QueryInterface(IID_PPV_ARGS(&identity))", own)
        self.assertIn("trace.resources.size() >= 1 + 2 * MaxNgxEndpoints", own)
        self.assertIn("trace.resourceBytes > MaxEndpointResourceBytes - bytes", own)
        self.assertIn("trace.resources.push_back(identity)", own)
        self.assertIn("identity.Get() == trace.resources.front().Get()", own)
        for field in ("dimension", "flags", "format", "width", "height", "mip_levels",
                      "depth_or_array_size", "sample_count", "sample_quality"):
            self.assertIn('"' + field + '"', own)
        observe = function("ObserveNgxInput")
        self.assertIn("trace->count >= MaxNgxEndpoints", observe)
        self.assertIn("trace->count == MaxNgxEndpoints", observe)
        self.assertIn("endpointActive.store(false", observe)
        self.assertIn("data.endpoint.reset()", observe)

    def test_endpoint_trace_does_not_claim_contents_or_gpu_frame_order(self):
        observe = function("ObserveNgxInput")
        for statement in ('{ "rr_frame_association", "not_established" }',
                          '{ "gpu_execution_order", "not_observed" }',
                          '{ "intervening_writes", "not_tracked" }',
                          '{ "unchanged_contents", "not_verified" }',
                          '"resource pointers only; mip/array/plane/subrect base not observed"'):
            self.assertIn(statement, observe)
        self.assertIn("CPU endpoint callbacks only; not submission or presentation order", observe)
        self.assertIn("Ordinal of observed endpoints only", SOURCE)

    def test_two_candidate_tokens_are_exact_endpoint_owned_not_queued_requests(self):
        self.assertIn("MaxCaptureCandidates = 2", SOURCE)
        observe = function("ObserveNgxInput")
        self.assertIn("trace->candidates.size() < MaxCaptureCandidates && recordingKnown && color", observe)
        self.assertIn('record["color"]["matches_fog_resource"].get<bool>()', observe)
        self.assertIn("candidate->ownership = trace", observe)
        self.assertIn("trace->candidates.push_back", observe)
        self.assertNotIn("FSRDResearch::Request", observe)
        self.assertIn('"unvalidated_candidate"', observe)
        for field in ("session_key", "candidate_index", "rr_feature", "rr_frame", "rr_recording_generation",
                      "rr_command_list_identity", "owned_resource_identity", "provenance_file"):
            self.assertIn('"' + field + '"', observe)
        result = function("CandidateCaptureResult")
        self.assertIn("if (!candidate || !candidate->ownership)", result)
        self.assertIn("if (state.reported)", result)
        self.assertIn("state.started = started", result)
        self.assertNotIn("FSRDResearch::", result)

    def test_submission_probe_is_bounded_and_only_matches_known_recording_generations(self):
        self.assertIn("MaxObservedSubmissions = 256, MaxListsPerSubmission = 512", SOURCE)
        self.assertIn("SubmissionWindowMs = 10000", SOURCE)
        prepare = function("PreparingSubmission")
        self.assertLess(prepare.index("if (!submissionActive.load"), prepare.index("QueryInterface("))
        self.assertIn("trace->observedSubmissions > MaxObservedSubmissions", prepare)
        self.assertIn("count > MaxListsPerSubmission", prepare)
        self.assertIn("trace->submissions.size() >= 8", prepare)
        self.assertIn("generation == trace->generation", prepare)
        self.assertIn("generation == trace->candidates[c].generation", prepare)
        self.assertIn("captureTrackingValid.load() && found != data.lists.end()", prepare)
        self.assertIn('"array_index"', prepare)
        for mutation in ("->Signal(", "->Wait(", "->ExecuteCommandLists(", "->ResourceBarrier("):
            self.assertNotIn(mutation, prepare + function("SubmittedSubmission"))

    def test_submission_intervals_do_not_invent_queue_order_or_frame_identity(self):
        relation = function("SubmissionRelation")
        self.assertIn("fog.submission->exit < rr.submission->entry", relation)
        self.assertIn("fog.submission->queue.Get() != rr.submission->queue.Get()", relation)
        for label in ("same_batch_array_order_only", "different_queues_synchronization_not_observed",
                      "ordered_nonoverlapping_same_queue_calls", "overlapping_queue_calls_order_unknown",
                      "repeated_recording_submission_ambiguous"):
            self.assertIn('"' + label + '"', relation)
        self.assertIn('{ "same_frame", "not_established" }', relation)
        self.assertIn('{ "resource_dependencies", "not_tracked" }', relation)

    def test_submission_sidecar_requires_closed_observations_and_is_not_positive_pairing(self):
        finalize = function("FinalizeSubmissionTrace")
        self.assertIn("candidate.reported", finalize)
        self.assertIn("fog.occurrences == 1 && fog.submission->exit", finalize)
        self.assertIn("rr.occurrences == 1 && rr.submission->exit", finalize)
        self.assertIn("trace->finalized = true", finalize)
        self.assertIn("ready && trace->failure.empty()", finalize)
        self.assertIn("candidate_provenance_only_not_validated_pairing", finalize)
        self.assertIn("CREATE_NEW", finalize)
        self.assertIn("MOVEFILE_WRITE_THROUGH", finalize)
        self.assertNotIn("MOVEFILE_REPLACE_EXISTING", finalize)
        self.assertIn("contents.size() > 2 * 1024 * 1024", finalize)
        for field in ("session_key", "fog_capture_id", "fog_origin", "candidates", "submissions",
                      "call_entry_serial", "call_exit_serial", "queue_identity"):
            self.assertIn('"' + field + '"', finalize)

    def test_both_queue_hook_paths_bracket_original_without_serializing_it(self):
        queue = (ROOT / "OptiScaler/resource_tracking/ResTrack_dx12.cpp").read_text()
        self.assertEqual(queue.count("FSRDCyberpunkFogProbe::PreparingSubmission(This, NumCommandLists, ppCommandLists)"), 2)
        self.assertEqual(queue.count("FSRDCyberpunkFogProbe::SubmittedSubmission(fogSubmission)"), 2)
        pattern = (r"const auto privateResetSubmission = FSRDCyberpunkFogProbe::AdmitPrivateResetSubmission"
                   r"\(This, NumCommandLists, ppCommandLists\);\s*"
                   r"const auto fogSubmission = FSRDCyberpunkFogProbe::PreparingSubmission"
                   r"\(This, NumCommandLists, ppCommandLists\);\s*"
                   r"const auto fsrdSubmission = FSRDSubmission::Preparing\(NumCommandLists, ppCommandLists\);\s*"
                   r"o_ExecuteCommandLists\(This, NumCommandLists, ppCommandLists\);\s*"
                   r"FSRDCyberpunkFogProbe::ReturnedPrivateResetSubmission\(privateResetSubmission\);\s*"
                   r"FSRDCyberpunkFogProbe::SubmittedSubmission\(fogSubmission\);")
        self.assertEqual(len(re.findall(pattern, queue)), 2)


if __name__ == "__main__":
    unittest.main()
