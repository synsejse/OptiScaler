#pragma once
#include "replay_options.h"
#include "../../OptiScaler/upscalers/ffx/FSRDCyberpunkResetCamera.h"
#include <array>
#include <bit>
#include <limits>

namespace Replay
{
inline constexpr const char* NativeMode = "cyberpunk_native_reset_v1";
struct NativeReset
{
    FSRD::CyberpunkResetCamera::Parameters camera;
    // Exact CB_Packing wire layout, not a C++/DirectXMath matrix reinterpretation.
    std::array<uint32_t, 60> constants {};
    uint64_t providerId = 0;
};

template<size_t N>
std::array<uint32_t, N> words(const nlohmann::json& value)
{
    if (!value.is_array() || value.size() != N)
        throw std::runtime_error("Native RESET requires an exact raw-word array");
    std::array<uint32_t, N> result;
    for (size_t i = 0; i < N; ++i) result[i] = integer(value.at(i), 0, UINT32_MAX);
    return result;
}

inline bool nativeMode(const nlohmann::json& job)
{
    if (!job.contains("input_mode")) return false; // Existing converted mode is unchanged.
    if (job.at("input_mode") != NativeMode)
        throw std::runtime_error("Unknown replay input_mode");
    return true;
}

inline NativeReset parseNativeReset(const nlohmann::json& job)
{
    using namespace FSRD::CyberpunkResetCamera;
    if (!nativeMode(job) || job.contains("dispatch"))
        throw std::runtime_error("Native RESET constructs its dispatch from raw sources, not a supplied dispatch");
    const auto& raw = job.at("raw_reset");
    if (raw.at("delta_source") != "explicit_reset_control_not_captured_duration" ||
        !raw.at("delta_ms").is_number())
        throw std::runtime_error("Native RESET needs an explicitly labeled frame-duration control");
    const auto& id = raw.at("expected_provider_id");
    if (!id.is_number_integer() || id <= 0 || (id.is_number_integer() && !id.is_number_unsigned() && id < 0))
        throw std::runtime_error("Native RESET needs an explicit positive provider ID");
    NativeReset result;
    result.providerId = id.get<uint64_t>();
    const auto& j = raw.at("camera");
    Snapshot s;
    s.nativeView = words<16>(j.at("native_view_words"));
    s.inverseNativeView = words<16>(j.at("inverse_native_view_words"));
    s.nativeProjection = words<16>(j.at("native_projection_words"));
    s.depthProjection = words<16>(j.at("depth_projection_words"));
    s.lensOffset = words<2>(j.at("lens_offset_words"));
    s.jitterPixels = words<2>(j.at("jitter_pixel_words"));
    const auto size = words<2>(j.at("render_size"));
    const auto jitterSize = words<2>(j.at("jitter_size"));
    s.width = size[0]; s.height = size[1];
    s.jitterWidth = jitterSize[0]; s.jitterHeight = jitterSize[1];
    s.projectionFlags = uint8_t(integer(j.at("projection_flags"), 0, 255));
    const auto mv = words<2>(raw.at("motion_scale_words"));
    const std::array<float, 2> motion { std::bit_cast<float>(mv[0]), std::bit_cast<float>(mv[1]) };
    const float delta = raw.at("delta_ms").get<float>();
    const auto frame = integer(raw.at("frame_index"), 0, UINT32_MAX);
    if (!Build(s, motion, delta, frame, result.camera))
        throw std::runtime_error("Current-source RESET camera validation refused the native snapshot");
    // No provider defaults or partial controls in raw mode.
    if (parseOptions(job, s.width, s.height).settings.size() != 6)
        throw std::runtime_error("Native RESET requires all six explicit provider controls");
    size_t cursor = 0;
    const auto appendWords = [&](const auto& input) {
        for (auto word : input) result.constants.at(cursor++) = word;
    };
    const auto appendFloats = [&](const auto& input) {
        for (float value : input) result.constants.at(cursor++) = std::bit_cast<uint32_t>(value);
    };
    appendWords(result.camera.inverseView);
    appendWords(result.camera.inverseProjection);
    appendWords(result.camera.previousView);
    appendFloats(result.camera.previousDepthProjection);
    appendFloats(result.camera.renderSize);
    result.constants.at(cursor++) = std::bit_cast<uint32_t>(result.camera.nearPlane);
    result.constants.at(cursor++) = std::bit_cast<uint32_t>(result.camera.farPlane);
    result.constants.at(cursor++) = result.camera.conversionFlags;
    result.constants.at(cursor++) = 0;
    if (cursor != 60 || result.constants[58] != 69)
        throw std::runtime_error("Native RESET conversion layout disagreement");
    return result;
}

inline nlohmann::json nativeDispatch(const NativeReset& raw)
{
    const auto& c = raw.camera;
    return { { "render_size", { uint32_t(c.renderSize[0]), uint32_t(c.renderSize[1]) } },
        { "frame_index", c.frameIndex }, { "flags", c.dispatchFlags },
        { "motion_scale", c.motionScale }, { "jitter", c.jitterNdc },
        { "camera_delta", c.cameraPositionDelta }, { "camera_right", c.cameraRight },
        { "camera_up", c.cameraUp }, { "camera_forward", c.cameraForward },
        { "aspect", c.aspectRatio }, { "near", c.nearPlane }, { "far", c.farPlane },
        { "fov", c.verticalFovRadians }, { "delta_ms", c.deltaMilliseconds } };
}
} // namespace Replay
