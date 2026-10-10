#include "cpu.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

using namespace std;

/* must read something like this
user  nice  system  idle  iowait  irq  softirq  steal
cpu   40604  776    31003  8540127 11383   0     16146 0 0 0
cpu0  4975    0     4518   1064452 2577    0     8085 0 0 0
cpu1  4832   71     3570   1068762  1113   0     3386 0 0 0
*/
cpuInfo readCpuInfo() {
    cpuInfo info;
    ifstream file("/proc/stat");
    string line;

    getline(file, line);

    istringstream iss(line);
    string key;

    iss >> key
        >> info.user >> info.nice >> info.system 
        >> info.idle >> info.iowait >> info.irq 
        >> info.softirq >> info.steal;
    
    return info;
}

    double cpuUsagePercent(const cpuInfo& before, const cpuInfo& after) {
        
        long idleBefore = before.idle + before.iowait;
        long idleAfter = after.idle + after.iowait;

        long totalBefore = before.user + before.nice + before.system + before.idle + before.irq + before.softirq + before.steal;
        long totalAfter = after.user + after.nice + after.system + after.idle + after.irq + after.softirq + after.steal;

        long deltaTotal = totalAfter - totalBefore;
        long deltaIdle = idleAfter - idleBefore;
        
        if(deltaTotal == 0) {
            return 0.0;
        }

        return (1.0 - static_cast<double>(deltaIdle) / deltaTotal) * 100.0;
    }