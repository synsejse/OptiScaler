#pragma once

#include "FSRDCyberpunkResetCamera.h"
#include <json.hpp>
#include <stdexcept>

// Narrow current-source adapter shared by the opt-in live RESET host and CPU
// tests. These are CPU source checks, never permission to read a native texture
// or proof of GPU ordering. No late NGX camera, tolerant match or frame offset.
namespace FSRD::CyberpunkPrivateResetSource
{
using Json = nlohmann::json;
inline void Require(bool condition)
{
    if (!condition) throw std::runtime_error("private RESET current camera/frame source refused");
}
inline uint32_t Word(const Json& value)
{
    Require(value.is_number_integer() && value >= 0 && value <= UINT32_MAX);
    return value.get<uint32_t>();
}
inline uintptr_t Address(const Json& value)
{
    Require(value.is_number_integer());
    if (!value.is_number_unsigned()) Require(value.get<int64_t>() > 0);
    const auto address = value.get<uint64_t>();
    Require(address != 0 && address <= UINTPTR_MAX);
    return uintptr_t(address);
}
template<size_t Rows, size_t Columns>
std::array<uint32_t, Rows * Columns> Words(const Json& field, uint32_t offset)
{
    Require(field.at("status") == "CPU_value_present" && field.at("view_offset") == offset &&
        field.at("layout") == "consecutive_float32_rows" && field.at("repeated_source_words_equal") == true &&
        field.at("all_finite") == true);
    const auto& rows = field.at("source_uint32_rows");
    Require(rows.is_array() && rows.size() == Rows);
    std::array<uint32_t, Rows * Columns> result {};
    for (size_t r = 0; r < Rows; ++r)
    {
        Require(rows[r].is_array() && rows[r].size() == Columns);
        for (size_t c = 0; c < Columns; ++c)
        {
            result[r * Columns + c] = Word(rows[r][c]);
            Require((result[r * Columns + c] & 0x7f800000u) != 0x7f800000u);
        }
    }
    return result;
}
inline uint32_t Scalar(const Json& field, uint32_t offset, bool bits = false)
{
    Require(field.at("status") == "CPU_value_present" && field.at("view_offset") == offset);
    return Word(field.at(bits ? "source_bits" : "source_value"));
}
struct Source
{
    uintptr_t view = 0, object = 0;
    uint32_t frame = 0, width = 0, height = 0;
    Json camera; // Includes all repeated authored source evidence, not just dispatch fields.
    CyberpunkResetCamera::Snapshot rawSnapshot {}; // Actual current source words, not reconstructed history.
    std::array<float, 2> motionScale {};
    CyberpunkResetCamera::Parameters parameters; // Legacy independently RESET template, even in ParseTemporal.
    bool SameFrame(const Source& other) const
    {
        return view == other.view && object == other.object && frame == other.frame &&
            width == other.width && height == other.height && camera == other.camera;
    }
};
inline Source Parse(const Json& metadata, float explicitResetDelta)
{
    Require(metadata.at("schema") == "optiscaler.fsr_rr.early_guide_availability.v1" &&
        metadata.at("status") == "CPU_metadata_only");
    Source result;
    result.view = Address(metadata.at("view"));
    const auto& dimensions = metadata.at("view_dimensions");
    Require(dimensions.is_array() && dimensions.size() == 2);
    result.width = Word(dimensions[0]); result.height = Word(dimensions[1]);
    const auto& camera = metadata.at("camera_provenance");
    Require(result.view && camera.at("schema") == "optiscaler.fsr_rr.early_camera_sources.v2" &&
        camera.at("status") == "current_view_CPU_sources_only" && camera.at("view_pointer_unchanged") == true);
    const auto& route = camera.at("frame_id_virtual_route");
    const auto& frame = route.at("explicit_frame_id_source");
    result.object = Address(route.at("object_address"));
    Require(result.object && result.object <= UINTPTR_MAX - 0x1b0 &&
        route.at("status") == "image_local_target_observed" && route.at("target_rva") == 0x18ec810 &&
        frame.at("status") == "CPU_value_present" && frame.at("getter_rva") == 0x18ec810 &&
        frame.at("source_object_offset") == 0x1b0 && frame.at("byte_size") == 4 &&
        frame.at("repeated_source_fields_equal") == true &&
        frame.at("semantics") == "CPU_source_for_later_explicit_Streamline_frame_ID" &&
        Address(frame.at("source_address")) == result.object + 0x1b0);
    result.frame = Word(frame.at("source_value"));
    auto& source = result.rawSnapshot;
    const auto& matrices = camera.at("matrices");
    source.nativeView = Words<4, 4>(matrices.at("native_view"), 0xc0);
    source.inverseNativeView = Words<4, 4>(matrices.at("inverse_native_view"), 0x180);
    source.nativeProjection = Words<4, 4>(matrices.at("native_projection_jittered"), 0x200);
    source.depthProjection = Words<4, 4>(matrices.at("depth_converted_projection_jittered"), 0x360);
    source.lensOffset = Words<1, 2>(camera.at("authored_lens_offset"), 0xa0);
    source.jitterPixels = { Scalar(camera.at("jitter_x"), 0x3e0, true), Scalar(camera.at("jitter_y"), 0x3e4, true) };
    source.width = result.width; source.height = result.height;
    source.jitterWidth = Scalar(camera.at("native_jitter_width"), 0x3e8);
    source.jitterHeight = Scalar(camera.at("native_jitter_height"), 0x3ec);
    const auto flags = Scalar(camera.at("projection_flags"), 0x3f4);
    Require(flags <= 255); source.projectionFlags = uint8_t(flags);
    const auto& motion = camera.at("motion_scale");
    Require(motion.at("status") == "current_property_CPU_values" && motion.at("repeated_source_fields_equal") == true &&
        motion.at("defaults_used") == false && motion.at("section") == "DLSS" &&
        motion.at("names") == Json::array({ "MvecScaleX", "MvecScaleY" }) &&
        motion.at("value_rvas") == Json::array({ 0x3464dc0, 0x3464e10 }) &&
        motion.at("semantics") == "native_SL_normalized_motion_multiplier" &&
        motion.at("source_bits").is_array() && motion.at("source_bits").size() == 2);
    result.motionScale = { std::bit_cast<float>(Word(motion.at("source_bits")[0])),
                           std::bit_cast<float>(Word(motion.at("source_bits")[1])) };
    Require(CyberpunkResetCamera::Build(source, result.motionScale, explicitResetDelta, result.frame, result.parameters));
    result.camera = camera;
    return result;
}

struct TemporalSource
{
    Source current;
    bool nativeResetRequested = false; // Valid only on a successfully returned ParseTemporal result.
};

// Additive stricter source access, NOT temporal dispatch/history admission.
// Legacy Parse accepts old RESET captures without reset-byte repeat metadata.
// The caller supplies this frame's measured duration with its explicit timing
// provenance; no timing is recovered from the source or guessed here. Parameters
// still hold the old RESET template. A temporal caller passes rawSnapshot and
// motionScale to CyberpunkTemporalCamera with separately owned history/continuity.
inline TemporalSource ParseTemporal(const Json& metadata, float explicitCurrentDelta)
{
    TemporalSource result { Parse(metadata, explicitCurrentDelta) };
    const auto& history = result.current.camera.at("history");
    Require(history.at("status") == "CPU_value_present" && Word(history.at("view_offset")) == 0xef0 &&
        history.at("semantics") == "native_SL_reset_equals_zero" &&
        history.at("repeated_source_fields_equal").is_boolean() &&
        history.at("repeated_source_fields_equal").get<bool>() &&
        history.at("producer_reset_candidate").is_boolean());
    const auto byte = Word(history.at("source_byte"));
    Require(byte <= 255 && history.at("producer_reset_candidate").get<bool>() == (byte == 0));
    result.nativeResetRequested = byte == 0;
    return result;
}
} // namespace FSRD::CyberpunkPrivateResetSource
