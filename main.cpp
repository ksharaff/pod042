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
#include "loadout.h"

int main() {

    // inside main():
    ChipManager chips(4);

    // Try to restore the saved loadout; on the first run there's no file yet.
    if (!loadLoadout(chips, "loadout.json")) {
        chips.enable("NETWORK SCAN");   // default loadout: 2 of 4 slots
        chips.enable("LOG STREAM");
}

    // This one should be refused: no slots left.
    string chipName = "DISK MONITOR";
    ChipResult r = chips.enable(chipName);
    if (r == ChipResult::NotEnoughSlots) {
        std::cout << "["<< chipName << "]" << " install refused: not enough slots\n";
    }

    std::cout << "Slots: " << chips.usedSlots() << "/" << chips.totalSlots() << "\n";
    for (const Chip& c : chips.chips()) {       // the UI will do this to draw the chip list
        std::cout << (c.enabled ? "[ON] " : "[OFF] ")
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
    


    saveLoadout(chips, "loadout.json");

    return 0;
}