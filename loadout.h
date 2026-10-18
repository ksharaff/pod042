#pragma once
#include <string>
#include <vector>
#include "chips.h"

// Writes the list of enabled chips to a JSON file. Returns false on failure.
bool saveLoadout(const ChipManager& chips, const std::string& path);

// Restores a saved loadout: disables everything, then enables the saved chips
// (the slot budget still applies). Returns false if the file is missing or invalid.
bool loadLoadout(ChipManager& chips, const std::string& path);

// A built-in, named set of chips.
struct Preset {
    std::string name;                // e.g. "SECURITY"
    std::vector<std::string> chips;  // chip names, e.g. {kChipNetwork, ...}
};

// The built-in presets, in the order the UI lists them (keys 1, 2, 3, ...).
const std::vector<Preset>& presets();

// Switches the chip manager to the named preset (everything else is turned off).
// Returns false, changing nothing, if no preset has that name.
bool applyPreset(ChipManager& chips, const std::string& name);

// The loadout used on the very first run, when no loadout.json exists yet.
void applyDefaultLoadout(ChipManager& chips);