#include "profile.h"
#include "log.h"
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

bool loadName(std::string& name, const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    // false = don't throw on bad JSON
    json j = json::parse(file, nullptr, false);

    
    if (j.is_discarded() || !j.contains("name") || !j["name"].is_string()) {
        logEvent(LogLevel::Error, "Profile file is invalid: " + path);
        return false;
    }

    std::string value = j["name"].get<std::string>();
    if (value.empty()) {
        return false;
    }

    name = value;  
    return true;
}

bool saveName(const std::string& name, const std::string& path) {
    json j;
    j["name"] = name;

    std::ofstream file(path);
    if (!file.is_open()) {
        logEvent(LogLevel::Error, "Could not write profile file: " + path);
        return false;
    }

    
    file << j.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    logEvent(LogLevel::Info, "Operator profile saved");
    return true;
}