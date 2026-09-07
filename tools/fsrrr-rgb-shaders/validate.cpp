// Build-host validation of the exact immutable DXBC arrays embedded in OptiScaler.
// This runs with the native Windows SDK compiler/reflection implementation;
// Wine's IsSampleFrequencyShader stub must not substitute for this proof.
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

#include "FSRDFogRgbWrite_VS.h"
#include "FSRDFogRgbWrite_PS.h"

namespace
{
using Microsoft::WRL::ComPtr;

bool Validate(const void* bytes, size_t size, bool pixel)
{
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(bytes, size, IID_PPV_ARGS(&reflection))) || !reflection)
        return false;
    D3D11_SHADER_DESC desc {};
    if (FAILED(reflection->GetDesc(&desc)) ||
        D3D11_SHVER_GET_TYPE(desc.Version) != UINT(pixel ? D3D11_SHVER_PIXEL_SHADER : D3D11_SHVER_VERTEX_SHADER) ||
        D3D11_SHVER_GET_MAJOR(desc.Version) != 5 || D3D11_SHVER_GET_MINOR(desc.Version) != 0 ||
        bool(reflection->IsSampleFrequencyShader()) != pixel ||
        desc.InputParameters != 1 || desc.OutputParameters != (pixel ? 1u : 2u) ||
        desc.BoundResources != (pixel ? 1u : 0u))
        return false;
    D3D11_SIGNATURE_PARAMETER_DESC input {};
    if (FAILED(reflection->GetInputParameterDesc(0, &input))) return false;
    if (pixel)
    {
        if (!input.SemanticName || _stricmp(input.SemanticName, "TEXCOORD") != 0 || input.SemanticIndex != 0 ||
            input.SystemValueType != D3D_NAME_UNDEFINED || input.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32 ||
            input.Mask != 3 || input.Stream != 0) return false;
        D3D11_SHADER_INPUT_BIND_DESC texture {};
        if (FAILED(reflection->GetResourceBindingDesc(0, &texture)) || texture.Type != D3D_SIT_TEXTURE ||
            texture.BindPoint != 0 || texture.BindCount != 1 || texture.Dimension != D3D_SRV_DIMENSION_TEXTURE2D ||
            texture.ReturnType != D3D_RETURN_TYPE_FLOAT) return false;
        D3D11_SIGNATURE_PARAMETER_DESC output {};
        if (FAILED(reflection->GetOutputParameterDesc(0, &output)) || output.SystemValueType != D3D_NAME_TARGET ||
            output.SemanticIndex != 0 || output.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32 ||
            output.Mask != 15 || output.Stream != 0) return false;
    }
    else if (input.SystemValueType != D3D_NAME_VERTEX_ID || input.ComponentType != D3D_REGISTER_COMPONENT_UINT32 ||
             input.Mask != 1 || input.SemanticIndex != 0 || input.Stream != 0)
        return false;
    return true;
}
}

int main()
{
    if (!Validate(FSRDFogRgbWrite_VS_cso, sizeof(FSRDFogRgbWrite_VS_cso), false) ||
        !Validate(FSRDFogRgbWrite_PS_cso, sizeof(FSRDFogRgbWrite_PS_cso), true))
    {
        std::fputs("Pre-Fog RGB DXBC contract failed: native VS/PS stage, sample frequency or resource signature.\n", stderr);
        return 1;
    }
    // The validator must refuse a real non-sample-frequency shader as a PS.
    if (Validate(FSRDFogRgbWrite_VS_cso, sizeof(FSRDFogRgbWrite_VS_cso), true)) return 2;
    std::puts("Pre-Fog exact embedded DXBC validated: VS5.0, PS5.0 sample frequency, t0 texture2D, TEXCOORD0.");
    return 0;
}
