#include <iostream>
#include "disk.h"
#include <sys/statvfs.h>

DiskInfo readDiskInfo(const char* path) {
    DiskInfo info;

    struct statvfs s;

    if(statvfs(path, &s) != 0) {
        return info;
    }

    // statvfs reports sizes in blocks not bytes
    // unsigned long long so that the multiplication happens in 64 bits
    unsigned long long blockSize = s.f_frsize;

    info.totalBytes = s.f_blocks * blockSize;
    info.availableBytes = s.f_bavail * blockSize;

    return info;
}   