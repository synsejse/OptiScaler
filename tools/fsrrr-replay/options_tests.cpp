#include "replay_options.h"
#include <iostream>
#include <limits>

using json = nlohmann::json;

static void require(bool value)
{
    if (!value)
        throw std::runtime_error("Replay option contract failed");
}
static void rejects(const json& job)
{
    try { Replay::parseOptions(job, 1280, 720); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid replay options were accepted");
}
int main()
{
    try
    {
        const auto defaults = Replay::parseOptions(json::object(), 1280, 720);
        require(!defaults.debug && defaults.settings.empty());
        const auto overview = Replay::parseOptions({ { "debug_view", json::object() } }, 1280, 720);
        require(overview.debug && overview.debugMode == 0 && overview.viewportIndex == 0 &&
                overview.debugWidth == 1280 && overview.debugHeight == 720);
        for (uint32_t view = 0; view < 12; ++view)
        {
            const auto fullscreen = Replay::parseOptions({ { "debug_view", {
                { "mode", "fullscreen" }, { "viewport_index", view }, { "output_size", { 641, 359 } }
            } } }, 1280, 720);
            require(fullscreen.debug && fullscreen.debugMode == 1 && fullscreen.viewportIndex == view &&
                    fullscreen.debugWidth == 641 && fullscreen.debugHeight == 359);
        }
        const auto settings = Replay::parseOptions({ { "provider_settings", {
            { "1", 1.0f }, { "2", 1.0f }, { "3", 65504.0f },
            { "4", 50.0f }, { "5", 0.0f }, { "6", 0.01f }
        } } }, 1280, 720);
        require(!settings.debug && settings.settings.size() == 6 && settings.settings.at(3) == 65504.0f);
        for (const auto& value : { json(-1), json(12), json(0.5), json(true), json("1") })
            rejects({ { "debug_view", { { "viewport_index", value } } } });
        for (const auto& value : { json(0), json(8193), json(-1), json(1.5), json(true) })
            rejects({ { "debug_view", { { "output_size", { value, 720 } } } } });
        rejects({ { "debug_view", nullptr } });
        rejects({ { "debug_view", false } });
        rejects({ { "debug_view", { { "mode", "unknown" } } } });
        rejects({ { "debug_view", { { "viewport", 2 } } } });
        rejects({ { "debug_view", { { "output_size", { 1280 } } } } });
        rejects({ { "provider_settings", json::array() } });
        rejects({ { "provider_settings", { { "7", 1 } } } });
        rejects({ { "provider_settings", { { "01", 1 } } } });
        rejects({ { "provider_settings", { { "1", "1" } } } });
        rejects({ { "provider_settings", { { "1", std::numeric_limits<double>::infinity() } } } });
        rejects({ { "provider_settings", { { "1", std::numeric_limits<double>::quiet_NaN() } } } });
        rejects({ { "provider_settings", { { "1", 1e100 } } } });
        std::cout << "Replay debug/settings option tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
