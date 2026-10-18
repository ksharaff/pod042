#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "connections.h"
#include "log.h"
#include "process.h"

// One detected change, ready to be written to the log.
struct Change {
    LogLevel level = LogLevel::Info;
    std::string message;
    unsigned long long inode = 0;   // socket id, so the caller can name the owner; 0 = no socket
};

// Remembers the previous sample and reports what is NEW in the current one.
// It never reads /proc itself: the caller passes the data in, which keeps it testable.
class ChangeDetector {
public:
    // Compares the current TCP connections with the previous call.
    // The first call after construction or resetConnections() only records a baseline.
    std::vector<Change> checkConnections(const std::vector<Connection>& conns);

    // Reports program NAMES not seen before. The first call only records a baseline.
    std::vector<Change> checkProcesses(const std::vector<ProcessInfo>& procs);

    // Forget the old state (call when the chip is removed), so reinstalling it
    // starts from a fresh baseline instead of reporting everything as new.
    void resetConnections();
    void resetProcesses();

private:
    // Listeners: key "addr|port" -> display text "addr:port" (kept so we can say what closed).
    std::map<std::string, std::string> prevListeners_;
    // Established connections, as keys like "out|1.2.3.4|443" or "in|22|5.6.7.8".
    std::set<std::string> prevEstablished_;
    bool connPrimed_ = false;

    std::set<std::string> knownNames_;   // every program name seen so far
    bool procPrimed_ = false;
};

// True for 127.x.x.x, ::1 and IPv4-mapped loopback (::ffff:127.x.x.x).
bool isLoopbackAddress(const std::string& addr);

// Keeps at most maxItems changes (Errors first, then Warnings, then Info) and
// appends one "... and N more changes" line if anything was dropped.
void limitChanges(std::vector<Change>& changes, std::size_t maxItems);