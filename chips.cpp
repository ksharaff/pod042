#include "chips.h"
#include "log.h"

ChipManager::ChipManager(int totalSlots) : totalSlots_(totalSlots) {
    chips_ = {
        {"NETWORK SCAN", "Enables network throughput readings.", 1}, // uses 1 slot
        {"PROCESS WATCH", "Enables the process scanner", 2},
        {"LOG STREAM", "Enables the live log tail", 1},
        {"DISK MONITOR",  "Enables disk usage readings", 1},
        {"COMMS", "Enables the Pod-to-Pod message link", 1},
    };
}

Chip* ChipManager::find(const string& name){
    for(Chip& c : chips_) { // "Chip&" = reference, so we get the real chip, not a copy
        if(c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

int ChipManager::usedSlots() const {
    int used = 0;
    for(const Chip& c : chips_) {
        if(c.enabled) {
            used += c.cost;
        }
    }
    return used;
}

int ChipManager::totalSlots() const {
    return totalSlots_;
}

bool ChipManager::isEnabled(const string& name) const {
    for(const Chip& c : chips_){
        if(c.name==name) {
            return c.enabled;
        }
    }
    return false;
}

const vector<Chip>& ChipManager::chips() const {
    return chips_;
}

ChipResult ChipManager::enable(const string& name) {
    Chip* chip = find(name);
    if (chip == nullptr) {
        return ChipResult::NotFound;
    }
    if (chip->enabled) {
        return ChipResult::AlreadyInState;
    }

    // Would installing this chip go over the slot budget? If so, refuse.
    if (usedSlots() + chip->cost > totalSlots_) {
        logEvent(LogLevel::Warning, "Cannot install chip " + name + ": not enough slots");
        return ChipResult::NotEnoughSlots;
    }

    // Everything fits: install it.
    chip->enabled = true;
    logEvent(LogLevel::Info, "Chip Installed: " + name);
    return ChipResult::Ok;
}

ChipResult ChipManager::disable(const string& name) {
    Chip* chip = find(name);
    if(chip==nullptr) {
        return ChipResult::NotFound;
    }
    if(!chip->enabled) { // if chip is disabled
        return ChipResult::AlreadyInState;
    }
    chip->enabled = false;
    logEvent(LogLevel::Info, "Chip Removed: " + name);
    return ChipResult::Ok;
    }