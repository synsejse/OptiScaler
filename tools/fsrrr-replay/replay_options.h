#pragma once
#include <json.hpp>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>

// CPU-only request validation, performed before loading the provider or creating a device.
namespace Replay
{
struct Options
{
    bool debug = false;
    uint32_t debugMode = 0;
    uint32_t viewportIndex = 0;
    uint32_t debugWidth = 0;
    uint32_t debugHeight = 0;
    std::map<uint64_t, float> settings;
};

inline uint32_t integer(const nlohmann::json& value, uint32_t minimum, uint32_t maximum)
{
    if (!value.is_number_integer() || value < minimum || value > maximum)
        throw std::runtime_error("Replay option requires an in-range integer");
    return value.get<uint32_t>();
}

inline Options parseOptions(const nlohmann::json& job, uint32_t width, uint32_t height)
{
    Options result;
    result.debugWidth = width;
    result.debugHeight = height;
    if (job.contains("debug_view"))
    {
        const auto& debug = job.at("debug_view");
        if (!debug.is_object())
            throw std::runtime_error("debug_view must be an object, or omitted to disable debugging");
        for (const auto& item : debug.items())
            if (item.key() != "mode" && item.key() != "viewport_index" && item.key() != "output_size")
                throw std::runtime_error("Unknown debug_view option: " + item.key());
        const auto mode = debug.value("mode", std::string("overview"));
        if (mode != "overview" && mode != "fullscreen")
            throw std::runtime_error("debug_view mode must be overview or fullscreen");
        result.debug = true;
        result.debugMode = mode == "fullscreen" ? 1u : 0u;
        result.viewportIndex = integer(debug.value("viewport_index", nlohmann::json(0)), 0, 11);
        if (debug.contains("output_size"))
        {
            const auto& size = debug.at("output_size");
            if (!size.is_array() || size.size() != 2)
                throw std::runtime_error("debug_view output_size must contain width and height");
            result.debugWidth = integer(size.at(0), 1, 8192);
            result.debugHeight = integer(size.at(1), 1, 8192);
        }
    }
    if (job.contains("provider_settings"))
    {
        const auto& settings = job.at("provider_settings");
        if (!settings.is_object())
            throw std::runtime_error("provider_settings must be an object");
        for (const auto& item : settings.items())
        {
            if (item.key().size() != 1 || item.key()[0] < '1' || item.key()[0] > '6' || !item.value().is_number())
                throw std::runtime_error("provider_settings supports numeric keys 1 through 6 only");
            const float value = item.value().get<float>();
            if (!std::isfinite(value))
                throw std::runtime_error("provider_settings values must be finite FP32");
            result.settings[uint64_t(item.key()[0] - '0')] = value;
        }
    }
    return result;
}
}
