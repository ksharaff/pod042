#include "cpu.h"
#include <algorithm>   // std::min
#include <cctype>      // std::isdigit
#include <fstream>
#include <sstream>

bool parseCpuLine(const std::string& line, std::string& label, cpuInfo& out) {
    std::istringstream iss(line);
    std::string key;
    cpuInfo info;   // filled locally; copied to "out" only if the whole line parsed

    // One >> per column, in the same order as /proc/stat.
    iss >> key >> info.user >> info.nice >> info.system >> info.idle
        >> info.iowait >> info.irq >> info.softirq >> info.steal;
    if (iss.fail()) {
        return false;
    }
    label = key;
    out = info;
    return true;
}

cpuInfo readCpuInfo() {
    cpuInfo info;
    std::ifstream file("/proc/stat");
    std::string line;
    std::string label;
    if (std::getline(file, line)) {   // the aggregate line is always first
        parseCpuLine(line, label, info);
    }
    return info;
}

std::vector<cpuInfo> readCoreCpuInfo() {
    std::vector<cpuInfo> cores;
    std::ifstream file("/proc/stat");
    std::string line;

    while (std::getline(file, line)) {
        // Per-core lines start with "cpu" followed by a digit ("cpu0 ...").
        // The total line is just "cpu ", and the lines after the CPU ones
        // ("intr", "ctxt", ...) don't start with "cpu" at all.
        if (line.size() < 4 || line.compare(0, 3, "cpu") != 0 ||
            !std::isdigit(static_cast<unsigned char>(line[3]))) {
            continue;
        }
        std::string label;
        cpuInfo info;
        if (parseCpuLine(line, label, info)) {
            cores.push_back(info);
        }
    }
    return cores;
}

double cpuUsagePercent(const cpuInfo& before, const cpuInfo& after) {
    // Idle time = idle + iowait (the CPU wasn't doing work in either state).
    long idleBefore = before.idle + before.iowait;
    long idleAfter  = after.idle + after.iowait;

    // Total time = the sum of all 8 counters.
    long totalBefore = before.user + before.nice + before.system + before.idle
                     + before.iowait + before.irq + before.softirq + before.steal;
    long totalAfter  = after.user + after.nice + after.system + after.idle
                     + after.iowait + after.irq + after.softirq + after.steal;

    long deltaTotal = totalAfter - totalBefore;
    long deltaIdle  = idleAfter - idleBefore;

    if (deltaTotal == 0) return 0.0;   // avoid dividing by zero

    // Convert to double BEFORE dividing, or integer division gives 0.
    return (1.0 - static_cast<double>(deltaIdle) / static_cast<double>(deltaTotal)) * 100.0;
}

std::vector<double> coreUsage(const std::vector<cpuInfo>& before,
                              const std::vector<cpuInfo>& after) {
    const std::size_t count = std::min(before.size(), after.size());
    std::vector<double> result;
    result.reserve(count);   // allocate once, instead of growing while pushing
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(cpuUsagePercent(before[i], after[i]));
    }
    return result;
}