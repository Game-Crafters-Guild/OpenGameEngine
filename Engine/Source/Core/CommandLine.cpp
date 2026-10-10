#include "Core/CommandLine.h"
#include "Core/Application.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace GameEngine {

namespace {
    static bool ToBool(std::string_view v, bool defaultValue) {
        if (v.empty()) return defaultValue; // flag present with no value
        if (v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON") return true;
        if (v == "0" || v == "false" || v == "FALSE" || v == "off" || v == "OFF") return false;
        return defaultValue;
    }
}

EngineArgs ParseEngineArgs(int argc, char** argv) {
    EngineArgs args{};

    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i] ? argv[i] : ""};
        if (a.rfind("--engine-", 0) != 0) {
            continue; // not an engine flag
        }

        // --engine-disable-clr[=value]
        if (a.rfind("--engine-disable-clr", 0) == 0) {
            std::string_view v;
            auto eq = a.find('=');
            if (eq != std::string_view::npos) v = a.substr(eq + 1);
            args.DisableClr = ToBool(v, true);
            continue;
        }

        // --engine-assets=<path>
        if (a.rfind("--engine-assets=", 0) == 0) {
            auto v = a.substr(std::string_view("--engine-assets=").size());
            if (!v.empty()) {
                std::filesystem::path p{std::string(v)};
                std::error_code ec;
                auto abs = std::filesystem::absolute(p, ec);
                args.AssetDirectory = ec ? std::string{p.string()} : std::string{abs.string()};
            }
            continue;
        }
    }

    return args;
}

void ApplyEngineArgsToConfig(const EngineArgs& args, ApplicationConfig& cfg) {
    if (args.AssetDirectory) cfg.AssetDirectory = *args.AssetDirectory;
}

} // namespace GameEngine

