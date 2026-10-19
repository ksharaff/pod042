#pragma once
#include <string>
#include <vector>
#include "disk.h"

// The three load averages from /proc/loadavg: how many processes, on average,
// wanted a CPU over the last 1, 5 and 15 minutes.
struct LoadAvg {
    double one = 0.0;
    double five = 0.0;
    double fifteen = 0.0;
};

// Parses a line like "0.52 0.58 0.59 1/467 12345". Returns false for anything else.
bool parseLoadAvg(const std::string& line, LoadAvg& out);

LoadAvg readLoadAvg();          // zeros if /proc/loadavg is unreadable
double readUptimeSeconds();     // seconds since boot (first number of /proc/uptime); 0 on failure

// One mounted filesystem with its space usage.
struct MountInfo {
    std::string device;       // e.g. "/dev/sdd"
    std::string mountPoint;   // e.g. "/"
    std::string fsType;       // e.g. "ext4"
    DiskInfo disk;
    bool readOnly = false;   // mounted read-only ("ro" among the mount options)
};

// /proc/mounts escapes special characters as a backslash plus three octal digits:
// a space becomes "\040". This undoes that. Anything that isn't a complete escape is left as is.
std::string unescapeMountField(const std::string& field);

// True for filesystems on real storage. Virtual ones (proc, sysfs, tmpfs, cgroup, overlay,
// squashfs, ...) and network ones (nfs, cifs) are NOT listed: see the note in host.cpp.
bool isRealFilesystem(const std::string& fsType);

// Splits one /proc/mounts line into device, mount point and type. Returns false if malformed.
bool parseMountLine(const std::string& line, MountInfo& out);

// All real mounted filesystems with their usage (at most 32).
std::vector<MountInfo> readMounts();