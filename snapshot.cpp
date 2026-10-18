#include "snapshot.h"
#include <chrono>
#include <ctime>
#include <iomanip>   // std::put_time
#include <map>
#include <sstream>   // std::ostringstream: builds text like a stream
#include <thread>
#include <nlohmann/json.hpp>
#include "connections.h"
#include "cpu.h"
#include "disk.h"
#include "meminfo.h"
#include "net.h"
#include "process.h"

using json = nlohmann::json;

std::string takeSnapshotJson(const ChipManager& chips) {
    const bool netOn  = chips.isEnabled(kChipNetwork);
    const bool diskOn = chips.isEnabled(kChipDisk);
    const bool procOn = chips.isEnabled(kChipProcess);

    // ---- First samples ----
    cpuInfo cpuBefore = readCpuInfo();
    NetInfo netBefore = readNetInfo();
    ProcessSampler sampler;
    if (procOn) {
        sampler.sample(cpuBefore);   // the result is unused: this only "primes" the sampler
    }
    const auto start = std::chrono::steady_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // ---- Second samples ----
    cpuInfo cpuAfter = readCpuInfo();
    NetInfo netAfter = readNetInfo();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    json j;

    // ISO-style local timestamp, e.g. "2026-10-18T13:05:09".
    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::ostringstream stamp;
    stamp << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    j["timestamp"] = stamp.str();

    // Core systems: always included.
    j["cpu_percent"] = cpuUsagePercent(cpuBefore, cpuAfter);
    MemInfo mem = readMemInfo();
    // A braced list of {"key", value} pairs becomes a JSON object.
    j["memory"] = {{"total_kb", mem.totalKb}, {"available_kb", mem.availableKb}};

    json enabled = json::array();
    for (const Chip& c : chips.chips()) {
        if (c.enabled) {
            enabled.push_back(c.name);
        }
    }
    j["chips"] = {{"slots_used", chips.usedSlots()},
                  {"slots_total", chips.totalSlots()},
                  {"enabled", enabled}};

    // Chip-gated readings.
    if (diskOn) {
        DiskInfo d = readDiskInfo("/");
        j["disk"] = {{"total_bytes", d.totalBytes}, {"available_bytes", d.availableBytes}};
    }

    if (netOn) {
        NetRates r = netRates(netBefore, netAfter, seconds);
        j["network"] = {{"rx_bytes_per_sec", r.rxBytesPerSec},
                        {"tx_bytes_per_sec", r.txBytesPerSec}};

        // Which process owns each socket? Only looked up if PROCESS WATCH is installed.
        std::map<unsigned long long, SocketOwner> owners;
        if (procOn) {
            owners = readSocketOwners();
        }

        json conns = json::array();
        for (const Connection& c : readConnections()) {
            json item = {{"state", c.state},
                         {"local", c.localAddr},   {"local_port", c.localPort},
                         {"remote", c.remoteAddr}, {"remote_port", c.remotePort}};
            auto found = owners.find(c.inode);
            if (found != owners.end()) {
                item["owner"] = {{"pid", found->second.pid}, {"name", found->second.name}};
            }
            conns.push_back(item);
        }
        j["network"]["connections"] = conns;
    }

    if (procOn) {
        std::vector<ProcessInfo> procs = sampler.sample(cpuAfter);   // sorted, busiest first
        json top = json::array();
        for (std::size_t i = 0; i < procs.size() && i < 10; ++i) {
            json item = {{"pid", procs[i].pid},
                         {"name", procs[i].name},
                         {"cpu_percent", procs[i].cpuPercent},
                         {"rss_kb", procs[i].rssKb}};
            top.push_back(item);
        }
        j["processes"] = {{"count", procs.size()}, {"top", top}};
    }

    // The last two arguments make dump() replace invalid UTF-8 (process names can
    // contain odd bytes) with "?" instead of throwing an exception.
    return j.dump(2, ' ', false, json::error_handler_t::replace);
}