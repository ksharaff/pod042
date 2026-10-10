#include <iostream>
#include <chrono>
#include <thread>
#include <string>
#include "meminfo.h"
#include "cpu.h"
#include "disk.h"
#include "net.h"
#include "log.h"


int main() {

    // Logging
    logEvent(LogLevel::Info, "POD-042 Booting");

    // Memory Info
    MemInfo m = readMemInfo();
    std::cout << "Memory Used: " << (m.totalKb - m.availableKb) / 1024 << " MB\n";
    std::cout << "Memory Available: " << m.availableKb / 1024 << " MB\n" << std::endl;

    // CPU info
    cpuInfo cpu = readCpuInfo();
    std::cout << "user = " << cpu.user << ", idle = " << cpu.idle << "\n" << std::endl;

    // CPU Usage Percentage
    cpuInfo a = readCpuInfo(); // before
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    cpuInfo b = readCpuInfo(); // after
    double cpuResult = cpuUsagePercent(a, b);
    std::cout << "CPU Usage: " << cpuResult << "%\n";


    if(cpuResult > 80.0) {
        logEvent(LogLevel::Warning, "CPU usage high: " + std::to_string(cpuResult) + "%");
    }

    // Disk Usage Info
    DiskInfo disk = readDiskInfo("/");
    std::cout << "Disk: " << (disk.totalBytes - disk.availableBytes) / (1024ULL * 1024 * 1024) 
                          << " GB used of " << disk.totalBytes / (1024ULL * 1024 * 1024) 
                          << " GB " << "(" << disk.availableBytes / (1024ULL * 1024 * 1024) 
                          << " GB available)\n";

    // Net Info
    NetInfo net1 = readNetInfo();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // wait 1 sec

    NetInfo net2 = readNetInfo();
    NetRates r = netRates(net1, net2, 1.0);
    std::cout << "NET: download " << r.rxBytesPerSec / 1024 << " KB/s, upload " << r.txBytesPerSec / 1024 << " KB/s\n";
    

    return 0;
}