#pragma once
#include <d3d12.h>
#include <memory>
#include <string>

namespace FSRDResearch
{
struct Batch;
using Capture = std::shared_ptr<Batch>;
// Opt-in, file-triggered, bounded to 12 frames/process and two in-flight batches.
Capture Begin(ID3D12Device* device, ID3D12GraphicsCommandList* list, UINT width, UINT height, uint64_t feature,
              uint64_t frame, const std::string& metadata);
void Record(const Capture& capture, const char* name, ID3D12Resource* texture);
// Called AFTER the original ExecuteCommandLists, never from within Evaluate.
void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
void Poll();
} // namespace FSRDResearch
