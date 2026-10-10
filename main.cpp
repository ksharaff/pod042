#include <iostream>
#include "meminfo.h"
#include "cpu.h"
#include "disk.h"
#include <chrono>
#include <thread>

int main() {

    // Memory Info
    MemInfo m = readMemInfo();
    std::cout << "Memory Used: " << (m.totalKb - m.availableKb) / 1024 << " MB\n";
    std::cout << "Memory Available: " << m.availableKb / 1024 << " MB\n" << std::endl;

    // CPU info
    cpuInfo cpu = readCpuInfo();
    std::cout << "user = " << cpu.user << ", idle = " << cpu.idle << "\n" << std::endl;

    // CPU Usage Percentage
    cpuInfo a = readCpuInfo();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    cpuInfo b = readCpuInfo();
    std::cout << "CPU Usage: " << cpuUsagePercent(a, b) << "%\n";

    // Disk Usage Info
    DiskInfo disk = readDiskInfo("/");
    std::cout << "Disk: " << (disk.totalBytes - disk.availableBytes) / (1024ULL * 1024 * 1024) 
                          << " GB used of " << disk.totalBytes / (1024ULL * 1024 * 1024) 
                          << " GB " << "(" << disk.availableBytes / (1024ULL * 1024 * 1024) 
                          << " GB available)\n";

    return 0;
}