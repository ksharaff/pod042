#pragma once
#include <vector>
#include <string>

inline const std::string kChipNetwork = "NETWORK SCAN";
inline const std::string kChipDisk    = "DISK MONITOR";
inline const std::string kChipLog     = "LOG STREAM";
inline const std::string kChipProcess = "PROCESS WATCH";
inline const std::string kChipComms = "COMMS";

using namespace std;

struct Chip {
    string name;
    string description;
    int cost;
    bool enabled = false;
};

enum class ChipResult { Ok, NotFound, AlreadyInState, NotEnoughSlots };

class ChipManager {
public:
    // explicit stops accidental conversions
    explicit ChipManager(int totalSlots);

    ChipResult enable(const string& name);
    ChipResult disable(const string& name);

    // read-only
    int usedSlots() const;
    int totalSlots() const;
    bool isEnabled(const string& name) const;
    const vector<Chip>& chips() const; // read only view for UI

private:
    Chip* find(const string& name); // returns nullptr if not found
    int totalSlots_; // underscore means member variable
    vector<Chip> chips_; // vector is like ArrayList in Java
};