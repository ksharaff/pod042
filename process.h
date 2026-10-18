#pragma once
#include <map>
#include <string>
#include <vector>
#include "cpu.h"

// Replaces control characters (newlines, escape codes, ...) in a process name with '?'.
// Process names are chosen by the processes themselves, so they are untrusted input:
// without this, a process could forge log lines or mess up the terminal.
std::string sanitizeName(std::string name);

struct ProcessInfo {
    int pid = 0;
    std::string name;
    double cpuPercent = 0.0;
    unsigned long long rssKb = 0; // resident memory: RAM the process is really using
};

class ProcessSampler {
public:

    std::vector<ProcessInfo> sample(const cpuInfo& cpuNow);
    void reset();

private:
    std::map<int, unsigned long long> prevTicks_;
    unsigned long long prevTotal_ = 0;
    bool primed_ = false;
};

struct SocketOwner {
    int pid = 0;
    std::string name;
};

std::map<unsigned long long, SocketOwner> readSocketOwners();