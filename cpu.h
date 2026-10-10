#pragma once

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

cpuInfo readCpuInfo();
double cpuUsagePercent(const cpuInfo& before, const cpuInfo& after);
