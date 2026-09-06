#pragma once
#include <d3d12.h>
#include <memory>
#include <string>
#include <cstdint>

namespace FSRDResearch
{
struct Batch;
using Capture = std::shared_ptr<Batch>;
struct Status
{
    bool busy = false;
    bool queued = false;
    std::string message;
    std::string directory;
};
// The GUI requests one frame of the selected feature, independently of the INI.
// Only queued (not already recorded GPU) work can be cancelled.
bool Request(uint64_t feature);
void CancelRequest();
Status GetStatus();
bool WantsCapture(uint64_t feature);
// GUI: one frame per click. Legacy file trigger: two frames, at most twelve/process.
// Both paths share the same bounded GPU storage and completion fences.
Capture Begin(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT width, UINT height, uint64_t feature,
              uint64_t frame, const std::string& metadata);
void Record(const Capture& capture, const char* name, ID3D12Resource* texture, bool fullExtent = false);
// Backend output is UAV (or a configured restored state), unlike the readable inputs.
void RecordOutput(const Capture& capture, ID3D12Resource* texture, D3D12_RESOURCE_STATES state);
void Finish(const Capture& capture, bool evaluationSucceeded);
// Called AFTER the original ExecuteCommandLists, never from within Evaluate.
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void Poll();
} // namespace FSRDResearch
