#pragma once

struct MemInfo {
    long totalKb = 0;
    long availableKb = 0;
    long swapTotalKb = 0;   // 0 means no swap is configured
    long swapFreeKb = 0;
};

MemInfo readMemInfo();