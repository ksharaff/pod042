#include "pod.h"
#include <cstdio>

namespace {
// Above these values the Pod changes its tone.
const double kCpuLimit = 80.0;
const double kMemLimit = 90.0;
}  // namespace

std::string formatDuration(double seconds) {
    // Negative values (which should never happen) are clamped to 0.
    long total = seconds > 0.0 ? static_cast<long>(seconds) : 0;
    char buf[32];
    // %02ld = a long, padded with zeros to at least 2 digits.
    std::snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld",
                  total / 3600, (total % 3600) / 60, total % 60);
    return buf;
}

std::string podHeartbeat(double cpuPercent, double memPercent, double uptimeSeconds) {
    const std::string uptime = formatDuration(uptimeSeconds);
    char buf[200];

    if (cpuPercent > kCpuLimit) {
        std::snprintf(buf, sizeof(buf),
                      "POD 042: REPORT. PROCESSING LOAD EXCEEDS THRESHOLD. CPU %.1f%%. UPTIME %s.",
                      cpuPercent, uptime.c_str());
    } else if (memPercent > kMemLimit) {
        std::snprintf(buf, sizeof(buf),
                      "POD 042: REPORT. MEMORY RESERVES LOW. MEM %.0f%%. UPTIME %s.",
                      memPercent, uptime.c_str());
    } else {
        std::snprintf(buf, sizeof(buf),
                      "POD 042: HEARTBEAT. ALL SYSTEMS NOMINAL. CPU %.1f%%. MEM %.0f%%. UPTIME %s.",
                      cpuPercent, memPercent, uptime.c_str());
    }
    return buf;
}