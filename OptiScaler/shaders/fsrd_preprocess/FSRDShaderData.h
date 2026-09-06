#pragma once
#include "FSRDShaderUtils.h"
#include <array>

namespace FSRD
{
    namespace FloorSeed
    {
        constexpr UINT kBackBufferCount = 3;

        enum class Flags : uint32_t
        {
            None = 0,
            LinearDepth = (1 << 0)
        };

        struct alignas(16) Constants
        {
            XMFLOAT4X4 InvProjMatrix;
            XMFLOAT4 RenderSize;

            float NearPlane;
            float FarPlane;
            uint32_t Flags;

            float _Padding[1];
        };

        union Input
        {
            struct Data
            {
                ID3D12Resource* InColor;
                ID3D12Resource* InSpecAlbedo;
                ID3D12Resource* InDiffAlbedo;
                ID3D12Resource* InDepth;
            };

            // The number of D3D12 resources in the struct
            static constexpr uint32_t kCount = sizeof(Data) / sizeof(ID3D12Resource*);

            Data Resources;

            ID3D12Resource* AsArray[kCount];
        };

        union Output
        {
            struct Data
            {
                ID3D12Resource* OutColor;
                ID3D12Resource* OutEdges;
            };

            // The number of D3D12 resources in the struct
            static constexpr uint32_t kCount = sizeof(Data) / sizeof(ID3D12Resource*);

            Data Resources;

            ID3D12Resource* AsArray[kCount];
        };
    }

    namespace FloorFilter
    {
        constexpr UINT kPasses = 3;
        constexpr UINT kBackBufferCount = std::max(3 * (kPasses + 1), 1u);

        enum class Flags : uint32_t
        {
            None = 0
        };

        struct alignas(16) Constants
        {
            XMFLOAT4 DstTexSize;

            int32_t StepSize;
            uint32_t Flags;

            float _Padding[2];
        };

        union Input
        {
            struct Data
            {
                ID3D12Resource* InColor;
                ID3D12Resource* InEdgeGuide;
            };

            // The number of D3D12 resources in the struct
            static constexpr uint32_t kCount = sizeof(Data) / sizeof(ID3D12Resource*);

            Data Resources;

            ID3D12Resource* AsArray[kCount];
        };

        union Output
        {
            struct Data
            {
                ID3D12Resource* OutColor;
                ID3D12Resource* OutEdgeGuide;
            };

            // The number of D3D12 resources in the struct
            static constexpr uint32_t kCount = sizeof(Data) / sizeof(ID3D12Resource*);

            Data Resources;

            ID3D12Resource* AsArray[kCount];
        };
    }

    namespace Conversion
    {
        constexpr UINT kBackBufferCount = 3;

        /**
         * @brief Constant buffer data passed to the conversion shader.
         */
        struct alignas(16) Constants
        {
            XMFLOAT4X4 InvViewMatrix;  // DLSSD WorldToView^1 - Camera matrix
            XMFLOAT4X4 InvProjMatrix;  // DLSSD ViewToClip^-1 - Projection
            XMFLOAT4X4 PrevViewMatrix; // DLSSD WorldToView from last frame

            XMFLOAT4 RenderSize;

            float NearPlane; // Near < Far - IsInverted flag accounts for inversion
            float FarPlane;  // Near < Far - IsInverted flag accounts for inversion

            float FloorIsolation;
            uint32_t Flags;  // Dynamic configuration flags. See: ConfigFlags
        };

        union Input
        {
            struct Data
            {
                ID3D12Resource* InColor;         // RGB - NVSDK_NGX_Parameter_Color - HDR or SDR
                ID3D12Resource* InDepth;         // R - NVSDK_NGX_Parameter_Depth - 24/32bits
                ID3D12Resource* InMotionVectors; // RG - NVSDK_NGX_Parameter_MotionVectors - RG16/RG32
                ID3D12Resource* InNormals; // RGB: Normals, A: Roughness (Optional) - NVSDK_NGX_Parameter_GBuffer_Normals - RGB16_FLOAT/RG32_FLOAT
                ID3D12Resource* InRoughness;   // R - May be packed in normals. NVSDK_NGX_Parameter_GBuffer_Roughness
                ID3D12Resource* InSpecHitDist; // R - NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance - FP16/FP32
                ID3D12Resource* InDiffAlbedo;  // RGB - NVSDK_NGX_Parameter_GBuffer_DiffuseAlbedo - RGBA32
                ID3D12Resource* InSpecAlbedo;  // RGB - NVSDK_NGX_Parameter_GBuffer_SpecularAlbedo - RGBA32
                ID3D12Resource* InBiasMask;    // R8 - NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask

                ID3D12Resource* InBlurColor;
            };

            // The number of D3D12 resources in the struct
            static constexpr uint32_t kCount = sizeof(Data) / sizeof(ID3D12Resource*);

            Data Resources;

            ID3D12Resource* AsArray[kCount];
        };

        /**
         * @brief Output resources formatted for direct consumption by FSR Ray Regeneration.
         * All resources are automatically transitioned to SRV state after dispatch.
         */
        struct Output
        {
            struct Data
            {
                ComPtr<ID3D12Resource>
                    Radiance; // RGB: Demodulated combined lighting, A: Specular Ray Length - RGBA16_FLOAT
                ComPtr<ID3D12Resource>
                    FusedAlbedo; // RGB: max(specularAlbedo, diffuseAlbedo), A: unused - RGB10A2_UNORM
                ComPtr<ID3D12Resource>
                    Motion; // RG: TSR motion, B: abs(PrevLinearDepth) - abs(CurrentLinearDepth) - RGBA16_FLOAT
                ComPtr<ID3D12Resource> Normals; // RG: Octahedrally encoded normals, B: Linear Roughness, A: Material Type (Optional) - RGB10A2_UNORM
                ComPtr<ID3D12Resource> SpecAlbedo;  // RGB: Specular Albedo, A: unused in RR 1.1 - RGB10A2_UNORM
                ComPtr<ID3D12Resource> DiffAlbedo;  // RGB: Diffuse Albedo, A: unused in RR 1.1 - RGB10A2_UNORM
                ComPtr<ID3D12Resource> LinearDepth; // R - R32_FLOAT
                ComPtr<ID3D12Resource> SkipSignal;
            };

            static constexpr uint32_t kCount = 8;
            Data Resources {};

            // Explicit UAV order (u0..u7). No union aliasing of live ComPtr objects or manual destruction.
            std::array<ID3D12Resource*, kCount> GetRawResources() const
            {
                return { Resources.Radiance.Get(),    Resources.FusedAlbedo.Get(), Resources.Motion.Get(),
                         Resources.Normals.Get(),     Resources.SpecAlbedo.Get(),  Resources.DiffAlbedo.Get(),
                         Resources.LinearDepth.Get(), Resources.SkipSignal.Get() };
            }
        };
    }

    namespace Composition
    {
        constexpr UINT kBackBufferCount = 7;
        constexpr UINT kOutputCount = 1;

        struct alignas(16) Constants
        {
            XMFLOAT4 DstTexSize; // XY = Tex Size - ZW = 1 / XY

            float CorrelationBias; // Controls the contribution of stable elements to the final image
            uint32_t Flags;

            XMFLOAT2 SrcTexSize; // Active source extent for debug blits
        };  

        /**
         * @brief Resources used for composition after denoising
         */
        union Input
        {
            struct Data
            {
                ID3D12Resource* InDenoisedRadiance;
                ID3D12Resource* InFusedAlbedo;

                ID3D12Resource* InSkipSignal;
                ID3D12Resource* InRawColor;
                ID3D12Resource* InColorBeforeParticles; // NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles
            };

            // The number of D3D12 resources in the struct
            static constexpr uint32_t kCount = sizeof(Data) / sizeof(ID3D12Resource*);

            Data Resources;

            ID3D12Resource* AsArray[kCount];
        };
    }
    } // namespace FSRD
