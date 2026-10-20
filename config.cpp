#include "config.h"
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Helpers private to this file.
namespace {

// Most problem lines we keep; a broken file shouldn't produce hundreds of log lines.
const std::size_t kMaxProblems = 20;

// Text that came from the file is untrusted: clean it and shorten it before it goes into a message.
std::string shown(const std::string& text) {
    return sanitizeText(text, 40);
}

// Reads one numeric setting. If the key is absent, "target" is left alone.
// If it has the wrong type or is outside [minValue, maxValue], it is ignored
// (target keeps its current value) and a problem is recorded.
void readNumber(const json& j, const std::string& key, int minValue, int maxValue,
                double& target, std::vector<std::string>& problems) {
    if (!j.contains(key)) {
        return;
    }
    const json& value = j.at(key);   // at() is safe here: we just checked contains()

    const std::string range = std::to_string(minValue) + " and " + std::to_string(maxValue);
    if (!value.is_number()) {
        problems.push_back(key + " must be a number between " + range + ": ignored");
        return;
    }

    const double number = value.get<double>();
    // Written as "!(a && b)" on purpose: it is also true for NaN, which fails every comparison.
    if (!(number >= minValue && number <= maxValue)) {
        problems.push_back(key + " must be between " + range + ": ignored");
        return;
    }
    target = number;
}

// heartbeat_seconds: a whole number, either 0 (off) or 10..86400.
void readHeartbeat(const json& j, AppConfig& config, std::vector<std::string>& problems) {
    const std::string key = "heartbeat_seconds";
    if (!j.contains(key)) {
        return;
    }
    const json& value = j.at(key);

    if (!value.is_number_integer()) {
        problems.push_back(key + " must be a whole number (0 = off, or 10 to 86400): ignored");
        return;
    }
    const long long seconds = value.get<long long>();
    if (seconds == 0 || (seconds >= 10 && seconds <= 86400)) {
        config.heartbeatSeconds = static_cast<int>(seconds);
    } else {
        problems.push_back(key + " must be 0 (off) or between 10 and 86400: ignored");
    }
}

// expected_ports: an array of whole numbers 1..65535. Bad entries are skipped one by one.
void readPorts(const json& j, AppConfig& config, std::vector<std::string>& problems) {
    const std::string key = "expected_ports";
    if (!j.contains(key)) {
        return;
    }
    const json& list = j.at(key);
    if (!list.is_array()) {
        problems.push_back(key + " must be a list like [22, 443]: ignored");
        return;
    }

    std::set<unsigned int> ports;   // a set also removes duplicates for free
    for (const json& item : list) {
        if (!item.is_number_integer()) {
            problems.push_back(key + " entries must be whole numbers: skipped one");
            continue;
        }
        const long long port = item.get<long long>();
        if (port < 1 || port > 65535) {
            problems.push_back(key + ": port " + std::to_string(port) +
                               " is not between 1 and 65535: skipped");
            continue;
        }
        if (ports.size() >= kMaxExpectedPorts &&
            ports.count(static_cast<unsigned int>(port)) == 0) {
            problems.push_back(key + " is limited to " + std::to_string(kMaxExpectedPorts) +
                               " ports: the rest were ignored");
            break;
        }
        ports.insert(static_cast<unsigned int>(port));
    }
    config.expectedPorts = ports;
}

// comms_port: a whole number from 1024 to 65535 (ports below 1024 need root).
void readCommsPort(const json& j, AppConfig& config, std::vector<std::string>& problems) {
    const std::string key = "comms_port";
    if (!j.contains(key)) {
        return;
    }
    const json& value = j.at(key);
    if (!value.is_number_integer()) {
        problems.push_back(key + " must be a whole number between 1024 and 65535: ignored");
        return;
    }
    const long long port = value.get<long long>();
    if (port < 1024 || port > 65535) {
        problems.push_back(key + " must be between 1024 and 65535: ignored");
        return;
    }
    config.commsPort = static_cast<unsigned int>(port);
}

// comms_bind: an IPv4 address, e.g. "127.0.0.1" (this machine only) or "192.168.1.10" (its LAN address).
void readCommsBind(const json& j, AppConfig& config, std::vector<std::string>& problems) {
    const std::string key = "comms_bind";
    if (!j.contains(key)) {
        return;
    }
    const json& value = j.at(key);
    if (!value.is_string() || !isIpv4Address(value.get<std::string>())) {
        problems.push_back(key + " must be an IPv4 address like \"127.0.0.1\": ignored");
        return;
    }
    config.commsBind = value.get<std::string>();
}

// comms_connect: a list of "host:port" strings. Bad entries are skipped one by one.
void readCommsConnect(const json& j, AppConfig& config, std::vector<std::string>& problems) {
    const std::string key = "comms_connect";
    if (!j.contains(key)) {
        return;
    }
    const json& list = j.at(key);
    if (!list.is_array()) {
        problems.push_back(key + " must be a list like [\"192.168.1.20:4242\"]: ignored");
        return;
    }

    std::vector<HostPort> targets;
    for (const json& item : list) {
        HostPort hp;
        if (!item.is_string() || !parseHostPort(item.get<std::string>(), hp)) {
            problems.push_back(key + " entries must look like \"192.168.1.20:4242\": skipped one");
            continue;
        }

        bool duplicate = false;
        for (const HostPort& existing : targets) {
            if (existing.host == hp.host && existing.port == hp.port) {
                duplicate = true;
            }
        }
        if (duplicate) {
            problems.push_back(key + ": " + shown(item.get<std::string>()) + " is listed twice: skipped");
            continue;
        }
        if (targets.size() >= kMaxCommsTargets) {
            problems.push_back(key + " is limited to " + std::to_string(kMaxCommsTargets) +
                               " addresses: the rest were ignored");
            break;
        }
        targets.push_back(hp);
    }
    config.commsConnect = targets;
}

// comms_allow: a list of IPv4 addresses that may connect IN. Bad entries are skipped one by one.
void readCommsAllow(const json& j, AppConfig& config, std::vector<std::string>& problems) {
    const std::string key = "comms_allow";
    if (!j.contains(key)) {
        return;
    }
    const json& list = j.at(key);
    if (!list.is_array()) {
        problems.push_back(key + " must be a list like [\"192.168.1.20\"]: ignored");
        return;
    }

    std::set<std::string> hosts;
    for (const json& item : list) {
        if (!item.is_string() || !isIpv4Address(item.get<std::string>())) {
            problems.push_back(key + " entries must be IPv4 addresses like \"192.168.1.20\": skipped one");
            continue;
        }
        const std::string host = item.get<std::string>();
        if (hosts.size() >= kMaxCommsAllow && hosts.count(host) == 0) {
            problems.push_back(key + " is limited to " + std::to_string(kMaxCommsAllow) +
                               " addresses: the rest were ignored");
            break;
        }
        hosts.insert(host);
    }
    config.commsAllow = hosts;
}

}  // namespace

AppConfig parseConfig(const std::string& text, std::vector<std::string>& problems) {
    AppConfig config;   // starts out as all defaults

    // The third argument (false) means "don't throw on bad JSON"; we get a special
    // "discarded" value to check instead.
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        problems.push_back("config is not valid JSON: using defaults");
        return config;
    }
    if (!j.is_object()) {
        problems.push_back("config must be a JSON object like {\"cpu_alert_percent\": 90}: using defaults");
        return config;
    }

    readNumber(j, "gauge_warn_percent", 1, 100, config.gaugeWarnPercent, problems);
    readNumber(j, "cpu_alert_percent", 1, 100, config.cpuAlertPercent, problems);
    readNumber(j, "mem_alert_percent", 1, 100, config.memAlertPercent, problems);
    readNumber(j, "process_alert_percent", 1, 10000, config.processAlertPercent, problems);
    readHeartbeat(j, config, problems);
    readPorts(j, config, problems);
    readCommsPort(j, config, problems);
    readCommsBind(j, config, problems);
    readCommsConnect(j, config, problems);
    readCommsAllow(j, config, problems);

    // Typos are the most common config mistake, so report keys we don't know.
    static const std::set<std::string> kKnown = {
        "gauge_warn_percent", "cpu_alert_percent", "mem_alert_percent",
        "process_alert_percent", "heartbeat_seconds", "expected_ports",
        "comms_port", "comms_bind", "comms_connect", "comms_allow"};
    for (const auto& item : j.items()) {   // items() gives key/value pairs of an object
        if (kKnown.count(item.key()) == 0) {
            // The key came from a file, so clean it before it reaches the log
            // (otherwise a newline in a key could forge a log line).
            problems.push_back("unknown setting \"" + shown(item.key()) + "\": ignored");
        }
    }

    // A badly broken file shouldn't flood the log.
    if (problems.size() > kMaxProblems) {
        const std::size_t extra = problems.size() - kMaxProblems;
        problems.resize(kMaxProblems);
        problems.push_back("... and " + std::to_string(extra) + " more problems");
    }
    return config;
}

AppConfig loadConfig(const std::string& path, std::vector<std::string>& problems) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return AppConfig{};   // no file is the normal first-run situation
    }

    // Read at most one byte more than the limit, so an oversized file is detected
    // without loading all of it into memory.
    std::string text(kMaxConfigBytes + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));   // gcount = bytes actually read

    if (text.size() > kMaxConfigBytes) {
        problems.push_back("config file is larger than 64 KB: using defaults");
        return AppConfig{};
    }
    return parseConfig(text, problems);
}

std::string defaultConfigJson() {
    const AppConfig defaults;   // the struct's own default values: one source of truth
    json j;
    j["gauge_warn_percent"] = defaults.gaugeWarnPercent;
    j["cpu_alert_percent"] = defaults.cpuAlertPercent;
    j["mem_alert_percent"] = defaults.memAlertPercent;
    j["process_alert_percent"] = defaults.processAlertPercent;
    j["heartbeat_seconds"] = defaults.heartbeatSeconds;
    j["expected_ports"] = json::array();   // an empty list: []
    j["comms_port"] = defaults.commsPort;
    j["comms_bind"] = defaults.commsBind;
    j["comms_connect"] = json::array();
    j["comms_allow"] = json::array();
    return j.dump(2) + "\n";
}