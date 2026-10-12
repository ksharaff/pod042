#include "loadout.h"
#include "log.h"
#include <fstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Shorthand so we can write "json" instead of "nlohmann::json".
using json = nlohmann::json;

bool saveLoadout(const ChipManager& chips, const std::string& path) {
    json j;
    j["enabled"] = json::array();   // an empty JSON array: []

    // Collect the names of the enabled chips.
    for (const Chip& c : chips.chips()) {
        if (c.enabled) {
            j["enabled"].push_back(c.name);
        }
    }

    std::ofstream file(path);       // default mode overwrites the file
    if (!file.is_open()) {
        logEvent(LogLevel::Error, "Could not write loadout file: " + path);
        return false;
    }

    file << j.dump(2) << "\n";      // dump(2) = pretty-print with 2-space indent
    logEvent(LogLevel::Info, "Loadout saved: " + path);
    return true;
}

bool loadLoadout(ChipManager& chips, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;               // no file yet: the caller decides what to do
    }

    // The third argument (false) means "don't throw on bad JSON";
    // instead we get a special "discarded" value we can check for.
    json j = json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.contains("enabled") || !j["enabled"].is_array()) {
        logEvent(LogLevel::Error, "Loadout file is invalid: " + path);
        return false;
    }

    // Copy the chip names first, so we aren't looping over the chip list
    // while changing it.
    std::vector<std::string> names;
    for (const Chip& c : chips.chips()) {
        names.push_back(c.name);
    }
    for (const std::string& name : names) {
        chips.disable(name);        // already-disabled chips just return AlreadyInState
    }

    // Enable each saved chip. Unknown names or over-budget chips are refused
    // by enable() itself, so a hand-edited file can't break the slot limit.
    for (const auto& item : j["enabled"]) {
        if (item.is_string()) {
            chips.enable(item.get<std::string>());
        }
    }

    logEvent(LogLevel::Info, "Loadout loaded: " + path);
    return true;
}