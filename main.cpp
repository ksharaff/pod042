#include <iostream>
#include <chrono>
#include <thread>
#include <string>
#include "meminfo.h"
#include "cpu.h"
#include "disk.h"
#include "net.h"
#include "log.h"
#include "chips.h"

int main() {

    ChipManager chips(4);                       // 4 slots total

    chips.enable("NETWORK SCAN");               // uses 1 slot  -> 1/4
    chips.enable("PROCESS WATCH");              // uses 2 slots -> 3/4
    chips.enable("LOG STREAM");                 // uses 1 slot  -> 4/4

    // This one should be refused: no slots left.
    ChipResult r = chips.enable("DISK MONITOR");
    if (r == ChipResult::NotEnoughSlots) {
        std::cout << "Install refused: not enough slots\n";
    }

    std::cout << "Slots: " << chips.usedSlots() << "/" << chips.totalSlots() << "\n";
    for (const Chip& c : chips.chips()) {       // the UI will do this to draw the chip list
        std::cout << (c.enabled ? "[ON ] " : "[OFF] ")
                << c.name << " (cost " << c.cost << ")\n";
    }

    // Logging
    logEvent(LogLevel::Info, "POD-042 Booting");

    // Memory Info
    MemInfo m = readMemInfo();
    std::cout << "Memory Used: " << (m.totalKb - m.availableKb) / 1024 << " MB\n";
    std::cout << "Memory Available: " << m.availableKb / 1024 << " MB\n";

    // CPU info
    cpuInfo cpu = readCpuInfo();
    std::cout << "user = " << cpu.user << ", idle = " << cpu.idle << "\n";

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
    NetRates rates = netRates(net1, net2, 1.0);
    std::cout << "NET: download " << rates.rxBytesPerSec / 1024 << " KB/s, upload " << rates.txBytesPerSec / 1024 << " KB/s\n";
    

    return 0;
}