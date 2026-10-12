#pragma once
#include <string>
#include "chips.h"

// Writes the list of enabled chips to a JSON file. Returns false on failure.
bool saveLoadout(const ChipManager& chips, const std::string& path);

// loads lodout
bool loadLoadout(ChipManager& chips, const std::string& path);