#include "loadout.h"
#include "log.h"
#include <fstream>
#include <nlohmann/json.hpp>

// Shorthand so we can write "json" instead of "nlohmann::json".
using json = nlohmann::json;

namespace {

// Turns every chip off, then turns on the listed ones. The slot budget still
// applies: a chip that doesn't fit is refused by enable() itself, so a hand-edited
// file or a wrong preset can never break the limit.
void setEnabledChips(ChipManager& chips, const std::vector<std::string>& names) {
    // Copy the chip names first, so we aren't looping over the chip list
    // while changing it.
    std::vector<std::string> all;
    for (const Chip& c : chips.chips()) {
        all.push_back(c.name);
    }
    for (const std::string& name : all) {
        chips.disable(name);   // already-disabled chips just return AlreadyInState
    }
    for (const std::string& name : names) {
        chips.enable(name);
    }
}

}  // namespace

bool saveLoadout(const ChipManager& chips, const std::string& path) {
    json j;
    j["enabled"] = json::array();   // an empty JSON array: []

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

    std::vector<std::string> names;
    for (const auto& item : j["enabled"]) {
        if (item.is_string()) {
            names.push_back(item.get<std::string>());
        }
    }
    setEnabledChips(chips, names);

    logEvent(LogLevel::Info, "Loadout loaded: " + path);
    return true;
}

const std::vector<Preset>& presets() {
    // A function-local "static" is created once, the first time the function
    // runs, and lives until the program ends. Later calls reuse the same object.
    static const std::vector<Preset> all = {
        {"DIAGNOSTIC",  {kChipDisk, kChipLog, kChipProcess}},      // 1 + 1 + 2 = 4 slots
        {"EXPLORATION", {kChipNetwork, kChipDisk, kChipLog}},      // 1 + 1 + 1 = 3 slots
        {"SECURITY",    {kChipNetwork, kChipProcess, kChipLog}},   // 1 + 2 + 1 = 4 slots
    };
    return all;
}

bool applyPreset(ChipManager& chips, const std::string& name) {
    for (const Preset& p : presets()) {
        if (p.name == name) {
            setEnabledChips(chips, p.chips);
            logEvent(LogLevel::Info, "Preset applied: " + name);
            return true;
        }
    }
    return false;   // unknown name: nothing was changed
}

void applyDefaultLoadout(ChipManager& chips) {
    setEnabledChips(chips, {kChipNetwork, kChipLog});
}