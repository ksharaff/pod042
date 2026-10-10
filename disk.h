#pragma once

struct DiskInfo {
    unsigned long long totalBytes = 0;
    unsigned long long availableBytes = 0;

};

// "path" includes "/"
DiskInfo readDiskInfo(const char* path);