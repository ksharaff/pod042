#pragma once
#include <cstddef>
#include <set>
#include <string>
#include <vector>
#include "protocol.h"   // HostPort

// Limits that keep a (possibly hand-edited) config file from using unlimited memory.
inline constexpr std::size_t kMaxExpectedPorts = 256;
inline constexpr std::size_t kMaxConfigBytes = 64 * 1024;   // 64 KB
inline constexpr std::size_t kMaxCommsTargets = 8;
inline constexpr std::size_t kMaxCommsAllow = 16;

// All settings, with their defaults. Anything missing from config.json keeps its default.
struct AppConfig {
    double gaugeWarnPercent = 80.0;      // gauges turn amber at or above this
    double cpuAlertPercent = 80.0;       // system CPU warning (log + header)
    double memAlertPercent = 90.0;       // memory level the Pod heartbeat calls "low"
    double processAlertPercent = 80.0;   // per-process CPU warning; 100 = one full core
    int heartbeatSeconds = 60;           // 0 = heartbeat off
    std::set<unsigned int> expectedPorts;   // ports expected to listen on all interfaces

    // --- COMMS chip (messages between pods) ---
    unsigned int commsPort = 4242;           // the port we listen on (1024-65535)
    std::string commsBind = "127.0.0.1";     // the address we listen on
    std::vector<HostPort> commsConnect;      // pods we dial out to
    std::set<std::string> commsAllow;        // IPv4 addresses allowed to connect in
};

// Turns the text of a config file into settings. Never fails: a setting that is
// missing keeps its default, and a setting that is invalid is ignored and
// described in "problems" (one human-readable line each).
AppConfig parseConfig(const std::string& text, std::vector<std::string>& problems);

// Reads and parses a config file. A MISSING file is normal (defaults, no problem);
// an unreadable-as-JSON or oversized one is reported in "problems".
AppConfig loadConfig(const std::string& path, std::vector<std::string>& problems);

// A complete config file with every setting at its default value (for --init-config).
std::string defaultConfigJson();