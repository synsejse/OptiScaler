#pragma once

#include "DLSSDFeature.h"
#include <upscalers/dlss/CrossAdapterDLSSFeature_Dx12.h>

class CrossAdapterDLSSDFeatureDx12 final : public CrossAdapterNGXFeatureDx12, public DLSSDFeature
{
  protected:
    void ProcessNativeInitParams(NVSDK_NGX_Parameter* parameters) override;
    void ProcessNativeEvaluateParams(NVSDK_NGX_Parameter* parameters) override;
    void ReadNativeVersion() override;

  public:
    feature_version Version() override { return DLSSDFeature::Version(); }
    Upscaler GetUpscalerType() const final { return Upscaler::DLSSD; }

    CrossAdapterDLSSDFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters);
};
