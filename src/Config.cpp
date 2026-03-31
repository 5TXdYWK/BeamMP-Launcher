/*
 Copyright (C) 2024 BeamMP Ltd., BeamMP team and contributors.
 Licensed under AGPL-3.0 (or later), see <https://www.gnu.org/licenses/>.
 SPDX-License-Identifier: AGPL-3.0-or-later
*/

#include "Logger.h"
#include "Network/network.hpp"
#include "Options.h"
#include "Voice/VoiceManager.hpp"
#include "Utils.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
namespace fs = std::filesystem;

std::string Branch;
std::filesystem::path CachingDirectory = std::filesystem::path("./Resources");
bool deleteDuplicateMods = false;

void ParseConfig(const nlohmann::json& d) {
    if (d["Port"].is_number()) {
        options.port = d["Port"].get<int>();
    }
    // Default -1
    // Release 1
    // EA 2
    // Dev 3
    // Custom 3
    if (d["Build"].is_string()) {
        Branch = d["Build"].get<std::string>();
        for (char& c : Branch)
            c = char(tolower(c));
    }
    if (d.contains("CachingDirectory") && d["CachingDirectory"].is_string()) {
        CachingDirectory = std::filesystem::path(d["CachingDirectory"].get<std::string>());
        info(beammp_wide("Mod caching directory: ") + beammp_fs_string(CachingDirectory.relative_path()));
    }

    if (d.contains("Dev") && d["Dev"].is_boolean()) {
        bool dev = d["Dev"].get<bool>();
        options.verbose = dev;
        options.no_download = dev;
        options.no_launch = dev;
        options.no_update = dev;
    }

    if (d.contains(("DeleteDuplicateMods")) && d["DeleteDuplicateMods"].is_boolean()) {
        deleteDuplicateMods = d["DeleteDuplicateMods"].get<bool>();
    }

    auto settings = Voice::VoiceManager::Get().GetSettings();
    if (d.contains("Voice") && d["Voice"].is_object()) {
        const auto& voice = d["Voice"];
        if (voice.contains("Enabled") && voice["Enabled"].is_boolean()) {
            settings.enabled = voice["Enabled"].get<bool>();
        }
        if (voice.contains("PushToTalk") && voice["PushToTalk"].is_boolean()) {
            settings.push_to_talk = voice["PushToTalk"].get<bool>();
        }
        if (voice.contains("FollowSystemDefaults") && voice["FollowSystemDefaults"].is_boolean()) {
            settings.follow_system_defaults = voice["FollowSystemDefaults"].get<bool>();
        }
        if (voice.contains("InputDeviceId") && voice["InputDeviceId"].is_string()) {
            settings.input_device_id = voice["InputDeviceId"].get<std::string>();
        }
        if (voice.contains("OutputDeviceId") && voice["OutputDeviceId"].is_string()) {
            settings.output_device_id = voice["OutputDeviceId"].get<std::string>();
        }
        if (voice.contains("MicGain") && voice["MicGain"].is_number()) {
            settings.mic_gain = voice["MicGain"].get<float>();
        }
        if (voice.contains("OutputGain") && voice["OutputGain"].is_number()) {
            settings.output_gain = voice["OutputGain"].get<float>();
        }
        if (voice.contains("AttenuationNearMeters") && voice["AttenuationNearMeters"].is_number()) {
            settings.attenuation_near_meters = voice["AttenuationNearMeters"].get<float>();
        }
        if (voice.contains("AttenuationFarMeters") && voice["AttenuationFarMeters"].is_number()) {
            settings.attenuation_far_meters = voice["AttenuationFarMeters"].get<float>();
        }
        if (voice.contains("AttenuationSmoothing") && voice["AttenuationSmoothing"].is_number()) {
            settings.attenuation_smoothing = voice["AttenuationSmoothing"].get<float>();
        }
    }
    Voice::VoiceManager::Get().SetSettings(settings);

}

void ConfigInit() {
    if (fs::exists("Launcher.cfg")) {
        std::ifstream cfg("Launcher.cfg");
        if (cfg.is_open()) {
            auto Size = fs::file_size("Launcher.cfg");
            std::string Buffer(Size, 0);
            cfg.read(&Buffer[0], Size);
            cfg.close();
            nlohmann::json d = nlohmann::json::parse(Buffer, nullptr, false);
            if (d.is_discarded()) {
                fatal("Config failed to parse make sure it's valid JSON!");
            }
            ParseConfig(d);
        } else
            fatal("Failed to open Launcher.cfg!");
    } else {
        std::ofstream cfg("Launcher.cfg");
        if (cfg.is_open()) {
            cfg <<
                R"({
    "Port": 4444,
    "Build": "Default",
    "CachingDirectory": "./Resources",
    "Voice": {
        "Enabled": true,
        "PushToTalk": true,
        "FollowSystemDefaults": true,
        "InputDeviceId": "default",
        "OutputDeviceId": "default",
        "MicGain": 1.0,
        "OutputGain": 1.0,
        "AttenuationNearMeters": 10.0,
        "AttenuationFarMeters": 120.0,
        "AttenuationSmoothing": 0.2
    }
})";
            cfg.close();
        } else {
            fatal("Failed to write config on disk!");
        }
    }
}

void SaveVoiceConfig() {
    nlohmann::json cfg = nlohmann::json::object();
    if (fs::exists("Launcher.cfg")) {
        std::ifstream in("Launcher.cfg");
        if (in.is_open()) {
            const auto size = fs::file_size("Launcher.cfg");
            std::string buffer(size, 0);
            in.read(&buffer[0], static_cast<std::streamsize>(size));
            in.close();
            auto parsed = nlohmann::json::parse(buffer, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                cfg = parsed;
            }
        }
    }
    const auto settings = Voice::VoiceManager::Get().GetSettings();
    cfg["Voice"] = {
        { "Enabled", settings.enabled },
        { "PushToTalk", settings.push_to_talk },
        { "FollowSystemDefaults", settings.follow_system_defaults },
        { "InputDeviceId", settings.input_device_id },
        { "OutputDeviceId", settings.output_device_id },
        { "MicGain", settings.mic_gain },
        { "OutputGain", settings.output_gain },
        { "AttenuationNearMeters", settings.attenuation_near_meters },
        { "AttenuationFarMeters", settings.attenuation_far_meters },
        { "AttenuationSmoothing", settings.attenuation_smoothing }
    };

    std::ofstream out("Launcher.cfg", std::ios::trunc);
    if (!out.is_open()) {
        error("Failed to persist voice config to Launcher.cfg");
        return;
    }
    out << cfg.dump(4);
}
