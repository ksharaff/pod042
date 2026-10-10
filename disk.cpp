#include <iostream>
#include "disk.h"
#include <sys/statvfs.h>

DiskInfo readDiskInfo(const char* path) {
    DiskInfo info;

    struct statvfs s;

    if(statvfs(path, $s) != 0) {
        return info;
    } else {
        std::cout << "Could not find path."
    }
}