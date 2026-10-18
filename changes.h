#pragma once
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "connections.h"
#include "log.h"
#include "process.h"

// The most program names we remember. Names come from OTHER processes, which can
// invent new ones endlessly, so the list needs a ceiling.
// "inline constexpr" lets a header define a constant without breaking the
// one-definition rule when several .cpp files include it.
inline constexpr std::size_t kMaxKnownPrograms = 5000;

// One detected change, ready to be written to the log.
struct Change {
    LogLevel level = LogLevel::Info;
    std::string message;
    unsigned long long inode = 0;   // socket id, so the caller can name the owner; 0 = no socket
};

// Remembers the previous sample and reports what is NEW in the current one.
// It never reads /proc or files itself: the caller passes the data in, which keeps it testable.
class ChangeDetector {
public:
    // Compares the current TCP connections with the previous call.
    // The first call after construction or resetConnections() only records a baseline.
    std::vector<Change> checkConnections(const std::vector<Connection>& conns);

    // Reports program NAMES not seen before. The first call only records a baseline,
    // unless a saved list was loaded first (then new names are reported right away).
    std::vector<Change> checkProcesses(const std::vector<ProcessInfo>& procs);

    // Starts from a saved list of program names (for example from the last run).
    // A non-empty list counts as a baseline, so names missing from it are reported
    // on the very first sample. An empty list changes nothing: the first sample is
    // a silent baseline, as on a first run.
    void loadKnownPrograms(const std::set<std::string>& names);

    // Every program name seen so far (including loaded ones). Only ever grows.
    const std::set<std::string>& knownPrograms() const { return knownNames_; }

    // Forget the old connection state (call when the chip is removed), so
    // reinstalling it starts from a fresh baseline instead of reporting everything as new.
    void resetConnections();

    // The next process sample is a quiet baseline again. Known names are KEPT,
    // so nothing already seen is forgotten (and a saved list is never shrunk).
    void resetProcesses();

private:
    // Listeners: key "addr|port" -> display text "addr:port" (kept so we can say what closed).
    std::map<std::string, std::string> prevListeners_;
    // Established connections, as keys like "out|1.2.3.4|443" or "in|22|5.6.7.8".
    std::set<std::string> prevEstablished_;
    bool connPrimed_ = false;

    std::set<std::string> knownNames_;   // every program name seen so far
    bool procPrimed_ = false;
    bool capWarned_ = false;             // true once we logged "list is full"
};

// True for 127.x.x.x, ::1 and IPv4-mapped loopback (::ffff:127.x.x.x).
bool isLoopbackAddress(const std::string& addr);

// Keeps at most maxItems changes (Errors first, then Warnings, then Info) and
// appends one "... and N more changes" line if anything was dropped.
void limitChanges(std::vector<Change>& changes, std::size_t maxItems);