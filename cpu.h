#pragma once
#include <string>
#include <vector>

struct cpuInfo {
    long user = 0;
    long nice = 0;
    long system = 0;
    long idle = 0;
    long iowait = 0;
    long irq = 0;
    long softirq = 0;
    long steal = 0;
};


bool parseCpuLine(const std::string& line, std::string& label, cpuInfo& out);

// The total over all cores (the first line of /proc/stat).
cpuInfo readCpuInfo();

// One entry per core (the "cpu0", "cpu1", ... lines), in order.
std::vector<cpuInfo> readCoreCpuInfo();

// Compares two readings, returns CPU usage from 0.0 to 100.0.
double cpuUsagePercent(const cpuInfo& before, const cpuInfo& after);


std::vector<double> coreUsage(const std::vector<cpuInfo>& before, const std::vector<cpuInfo>& after);