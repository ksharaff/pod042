#include "meminfo.h"
#include <fstream>
#include <sstream>
#include <string>

MemInfo readMemInfo() {
    MemInfo info;
    std::ifstream file("/proc/meminfo");
    std::string line;

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string key;
        long value = 0;
        iss >> key >> value;   // e.g. "MemTotal:" then 16384000 (the "kB" is ignored)

        if (key == "MemTotal:")     info.totalKb = value;
        if (key == "MemAvailable:") info.availableKb = value;
        if (key == "SwapTotal:")    info.swapTotalKb = value;
        if (key == "SwapFree:")     info.swapFreeKb = value;
    }
    return info;
}