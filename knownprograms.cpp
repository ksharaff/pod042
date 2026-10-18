#include "knownprograms.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "changes.h"   // kMaxKnownPrograms
#include "log.h"
#include "process.h"   // sanitizeName

using json = nlohmann::json;

std::set<std::string> loadKnownPrograms(const std::string& path) {
    std::set<std::string> names;

    std::ifstream file(path);
    if (!file.is_open()) {
        return names;   // first run: no file yet
    }

    // false = don't throw on bad JSON; we get a "discarded" value to check instead.
    json j = json::parse(file, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("programs") ||
        !j["programs"].is_array()) {
        logEvent(LogLevel::Error, "Known-program list is invalid: " + path);
        return names;   // behave as if there was no file
    }

    for (const auto& item : j["programs"]) {
        if (!item.is_string()) {
            continue;   // ignore entries of the wrong type
        }
        if (names.size() >= kMaxKnownPrograms) {
            break;      // a hand-edited giant file can't make us use unlimited memory
        }
        // The file could have been edited by hand, so clean every name again.
        names.insert(sanitizeName(item.get<std::string>()));
    }
    return names;
}

bool saveKnownPrograms(const std::set<std::string>& names, const std::string& path) {
    json j;
    j["programs"] = json::array();
    for (const std::string& name : names) {   // a std::set is already sorted
        j["programs"].push_back(name);
    }

    const std::string tempPath = path + ".tmp";
    std::error_code ec;   // the filesystem calls below report problems through "ec" instead of throwing

    {
        // The braces make "file" close (and flush) at the closing brace, BEFORE the
        // rename below. Renaming a file that is still open for writing can lose data.
        std::ofstream file(tempPath);
        if (!file.is_open()) {
            logEvent(LogLevel::Error, "Could not write known-program list: " + tempPath);
            return false;
        }
        // The last argument replaces invalid UTF-8 with "?" instead of throwing.
        file << j.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
        file.flush();
        if (!file) {   // an ofstream converts to false if any write failed (e.g. disk full)
            logEvent(LogLevel::Error, "Writing the known-program list failed: " + tempPath);
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    // rename() replaces the old file in one step. Readers see either the old file
    // or the new one, never something in between.
    std::filesystem::rename(tempPath, path, ec);
    if (ec) {
        logEvent(LogLevel::Error, "Could not replace known-program list: " + path);
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
}