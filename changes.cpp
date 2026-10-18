#include "changes.h"
#include <algorithm>   // std::stable_sort

// Helpers private to this file.
namespace {

std::string listenKey(const Connection& c) {
    return c.localAddr + "|" + std::to_string(c.localPort);
}

// A listener on 0.0.0.0 or :: accepts connections on EVERY network interface.
bool isExposed(const std::string& addr) {
    return addr == "0.0.0.0" || addr == "::";
}

}  // namespace

bool isLoopbackAddress(const std::string& addr) {
    // rfind(text, 0) == 0 is the "starts with" trick.
    return addr.rfind("127.", 0) == 0 || addr == "::1" || addr.rfind("::ffff:127.", 0) == 0;
}

std::vector<Change> ChangeDetector::checkConnections(const std::vector<Connection>& conns) {
    // ---- Step 1: describe the CURRENT state ----
    // std::map keeps its keys sorted, so the order of reported changes is stable.
    std::map<std::string, Connection> listeners;
    std::set<unsigned int> listenPorts;
    for (const Connection& c : conns) {
        if (c.state == "LISTEN") {
            // emplace inserts only if the key is new; a duplicate key is ignored.
            listeners.emplace(listenKey(c), c);
            listenPorts.insert(c.localPort);
        }
    }

    std::map<std::string, Connection> established;
    for (const Connection& c : conns) {
        if (c.state != "ESTABLISHED" || isLoopbackAddress(c.remoteAddr)) {
            continue;
        }
        // If the local port is one we LISTEN on, someone connected to us: inbound.
        // Otherwise our side picked a random port: an outbound connection.
        const bool inbound = listenPorts.count(c.localPort) > 0;   // count() is 0 or 1 for a set

        // The key decides what counts as "the same connection" between samples.
        //   outbound: destination address + port (a browser opening ten sockets to the
        //             same server is one event, not ten)
        //   inbound:  our port + the client's address (the client's random port is ignored)
        const std::string key = inbound
            ? "in|" + std::to_string(c.localPort) + "|" + c.remoteAddr
            : "out|" + c.remoteAddr + "|" + std::to_string(c.remotePort);
        established.emplace(key, c);
    }

    // ---- Step 2: compare with the previous sample ----
    std::vector<Change> changes;
    if (connPrimed_) {   // the first sample is only a baseline
        // "const auto& [key, c]" is a STRUCTURED BINDING (C++17): it unpacks each
        // map entry (a key/value pair) into two named variables.
        for (const auto& [key, c] : listeners) {
            if (prevListeners_.count(key) > 0) {
                continue;   // already known
            }
            const std::string where = endpoint(c.localAddr, c.localPort);
            if (isExposed(c.localAddr)) {
                changes.push_back({LogLevel::Warning, "New listener on ALL interfaces: " + where, c.inode});
            } else {
                changes.push_back({LogLevel::Info, "New listener: " + where, c.inode});
            }
        }

        for (const auto& [key, where] : prevListeners_) {
            if (listeners.count(key) == 0) {
                changes.push_back({LogLevel::Info, "Listener closed: " + where, 0});
            }
        }

        for (const auto& [key, c] : established) {
            if (prevEstablished_.count(key) > 0) {
                continue;
            }
            if (key.rfind("in|", 0) == 0) {
                changes.push_back({LogLevel::Info,
                                   "New inbound connection: " + c.remoteAddr + " -> port " +
                                       std::to_string(c.localPort),
                                   c.inode});
            } else {
                changes.push_back({LogLevel::Info,
                                   "New outbound connection: " + endpoint(c.remoteAddr, c.remotePort),
                                   c.inode});
            }
        }
    }

    // ---- Step 3: remember the current state for next time ----
    prevListeners_.clear();
    for (const auto& [key, c] : listeners) {
        prevListeners_[key] = endpoint(c.localAddr, c.localPort);
    }
    prevEstablished_.clear();
    for (const auto& entry : established) {
        prevEstablished_.insert(entry.first);   // entry.first is the key
    }
    connPrimed_ = true;

    return changes;
}

std::vector<Change> ChangeDetector::checkProcesses(const std::vector<ProcessInfo>& procs) {
    std::vector<Change> changes;
    for (const ProcessInfo& p : procs) {
        if (p.name.empty()) {
            continue;
        }
        // set::insert returns a pair; its .second is true only if the name was NEW.
        const bool isNew = knownNames_.insert(p.name).second;
        if (isNew && procPrimed_) {   // names seen during the baseline are not reported
            changes.push_back({LogLevel::Info,
                               "New program started: " + p.name + " (pid " + std::to_string(p.pid) + ")",
                               0});
        }
    }
    procPrimed_ = true;
    return changes;
}

void ChangeDetector::resetConnections() {
    prevListeners_.clear();
    prevEstablished_.clear();
    connPrimed_ = false;
}

void ChangeDetector::resetProcesses() {
    knownNames_.clear();
    procPrimed_ = false;
}

void limitChanges(std::vector<Change>& changes, std::size_t maxItems) {
    if (changes.size() <= maxItems) {
        return;   // nothing to cut
    }

    // Most severe first, so the cut never throws away a warning to keep an info line.
    // stable_sort keeps the original order among changes of the same level.
    std::stable_sort(changes.begin(), changes.end(),
                     [](const Change& a, const Change& b) {
                         return static_cast<int>(a.level) > static_cast<int>(b.level);
                     });

    const std::size_t dropped = changes.size() - maxItems;
    changes.resize(maxItems);   // shrink to the first maxItems entries
    changes.push_back({LogLevel::Info, "... and " + std::to_string(dropped) + " more changes", 0});
}