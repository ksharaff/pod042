#include <iostream>
#include "meminfo.h"
#include "cpu.h"
#include <chrono>
#include <thread>

int main() {
    MemInfo m = readMemInfo();
    std::cout << "Used: " << (m.totalKb - m.availableKb) / 1024 << " MB\n";
    std::cout << "Free: " << m.availableKb / 1024 << " MB\n";

    cpuInfo cpu = readCpuInfo();
    std::cout << "user = " << cpu.user << ", idle = " << cpu.idle << "\n";

    cpuInfo a = readCpuInfo();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    cpuInfo b = readCpuInfo();
    std::cout << "CPU: " << cpuUsagePercent(a, b) << "%\n";

    return 0;
}