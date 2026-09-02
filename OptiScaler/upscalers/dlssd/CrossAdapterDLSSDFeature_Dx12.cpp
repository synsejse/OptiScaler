#include <pch.h>

#include "CrossAdapterDLSSDFeature_Dx12.h"

CrossAdapterDLSSDFeatureDx12::CrossAdapterDLSSDFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters)
    : IFeature(handleId, parameters),
      CrossAdapterNGXFeatureDx12(handleId, parameters, CrossAdapterNGXFeature::RayReconstruction),
      DLSSDFeature(handleId, parameters)
{
}

void CrossAdapterDLSSDFeatureDx12::ProcessNativeInitParams(NVSDK_NGX_Parameter* parameters)
{
    DLSSDFeature::ProcessInitParams(parameters);
}

void CrossAdapterDLSSDFeatureDx12::ProcessNativeEvaluateParams(NVSDK_NGX_Parameter* parameters)
{
    DLSSDFeature::ProcessEvaluateParams(parameters);
}

void CrossAdapterDLSSDFeatureDx12::ReadNativeVersion() { DLSSDFeature::ReadVersion(); }
