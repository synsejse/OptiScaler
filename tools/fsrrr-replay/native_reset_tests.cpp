#include "native_reset_options.h"
#include <cassert>
#include <iostream>
#include <fstream>

using nlohmann::json;
static json fixture()
{
    // Captured current CPU source words, early-guides-092958. Not a pairing claim.
    return { { "schema", 1 }, { "input_mode", Replay::NativeMode },
        { "provider_settings", { { "1", 1 }, { "2", 1 }, { "3", 65504 }, { "4", 50 }, { "5", 0 }, { "6", .01 } } },
        { "raw_reset", {
            { "delta_ms", 17.25 }, { "delta_source", "explicit_reset_control_not_captured_duration" },
            { "expected_provider_id", 123 }, { "frame_index", 209225 }, { "motion_scale_words", { 1065353216, 1065353216 } },
            { "camera", {
                { "native_view_words", { 1052756518u,977788298u,1064131678u,0u,3211615332u,966924020u,1052756514u,0u,
                    898473216u,1065353210u,3126307840u,0u,3302796298u,3262674531u,1161996442u,1065353216u } },
                { "inverse_native_view_words", { 1052756518u,3211615332u,898473216u,0u,977788298u,966924020u,1065353210u,0u,
                    1064131678u,1052756514u,3126307840u,0u,3305842410u,3308206053u,1115773974u,1065353216u } },
                { "native_projection_words", { 1066856116u,0u,0u,0u,0u,1074145668u,0u,0u,975385395u,982935142u,
                    1065353226u,1065353216u,0u,0u,3164854039u,0u } },
                { "depth_projection_words", { 1066856116u,0u,0u,0u,0u,1074145668u,0u,0u,975385395u,982935142u,
                    3047161856u,1065353216u,0u,0u,1017370391u,0u } },
                { "lens_offset_words", { 0, 0 } }, { "jitter_pixel_words", { 1053556736, 1054048256 } },
                { "render_size", { 1280, 720 } }, { "jitter_size", { 1280, 720 } }, { "projection_flags", 4 }
            } }
        } } };
}
int main(int argc, char** argv)
{
    // Also supplies a GPU-free actual-builder validator to the offline Python
    // preparer. No data conversion or provider/default lookup occurs here.
    if (argc == 2)
    {
        try
        {
            const auto request = json::parse(std::ifstream(argv[1]));
            const auto parsed = Replay::parseNativeReset(request);
            std::cout << json({ { "dispatch", Replay::nativeDispatch(parsed) },
                { "conversion_constants_words", parsed.constants } }).dump() << '\n';
            return 0;
        }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    if (argc != 1) return 1;
    auto job = fixture();
    const auto parsed = Replay::parseNativeReset(job);
    const auto dispatch = Replay::nativeDispatch(parsed);
    assert(parsed.providerId == 123 && parsed.constants[58] == 69 && parsed.constants[59] == 0);
    assert(dispatch["render_size"] == json::array({1280, 720}));
    assert(dispatch["flags"] == 3 && dispatch["frame_index"] == 209225 && dispatch["delta_ms"] == 17.25);
    assert(dispatch["camera_delta"] == json::array({0, 0, 0}));
    assert(dispatch["motion_scale"] == json::array({1, 1, 1}));
    const auto& c = parsed.camera;
    for (size_t i = 0; i < 16; ++i)
    {
        assert(parsed.constants[i] == c.inverseView[i]);
        assert(parsed.constants[16 + i] == c.inverseProjection[i]);
        assert(parsed.constants[32 + i] == c.previousView[i]);
    }
    for (size_t i = 0; i < 4; ++i)
    {
        assert(parsed.constants[48 + i] == std::bit_cast<uint32_t>(c.previousDepthProjection[i]));
        assert(parsed.constants[52 + i] == std::bit_cast<uint32_t>(c.renderSize[i]));
    }
    assert(parsed.constants[56] == std::bit_cast<uint32_t>(c.nearPlane));
    assert(parsed.constants[57] == std::bit_cast<uint32_t>(c.farPlane));
    auto refused = [](const json& bad) {
        bool rejected = false;
        try { (void)Replay::parseNativeReset(bad); } catch (const std::exception&) { rejected = true; }
        assert(rejected);
    };
    for (unsigned test = 0; test < 18; ++test)
    {
        auto bad = job;
        auto& raw = bad["raw_reset"];
        auto& cam = raw["camera"];
        switch (test)
        {
        case 0: bad["dispatch"] = json::object(); break;
        case 1: bad["input_mode"] = "guessed"; break;
        case 2: raw.erase("delta_ms"); break;
        case 3: raw["delta_ms"] = 0; break;
        case 4: raw["delta_source"] = "captured"; break;
        case 5: raw["expected_provider_id"] = 0; break;
        case 6: raw["expected_provider_id"] = -1; break;
        case 7: raw["frame_index"] = 4294967296ull; break;
        case 8: raw["motion_scale_words"] = {1065353216u, 0x7f800000u}; break;
        case 9: cam["native_view_words"][0] = true; break;
        case 10: cam["jitter_pixel_words"][1] = cam["jitter_pixel_words"][1].get<uint32_t>() ^ 0x80000000u; break;
        case 11: cam["jitter_size"] = {2560, 1440}; break;
        case 12: cam["lens_offset_words"] = {0x3e000000u, 0}; break;
        case 13: cam["projection_flags"] = 0; break;
        case 14: bad["provider_settings"].erase("6"); break;
        case 15: raw["delta_ms"] = 1e100; break;
        case 16: cam["native_view_words"][0] = -1; break;
        case 17: cam["native_view_words"].erase(0); break;
        }
        refused(bad);
    }
    auto legacy = job; legacy.erase("input_mode");
    assert(!Replay::nativeMode(legacy));
    for (auto word : parsed.constants) std::cout << word << ' ';
    std::cout << '\n';
}
