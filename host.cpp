#include "host.h"
#include <fstream>
#include <set>
#include <sstream>
#include "process.h"   // sanitizeName

namespace {
// A safety limit on how much work one call can do.
const std::size_t kMaxMounts = 32;

bool hasOption(const std::string& options, const std::string& wanted) {
    std::size_t start = 0;
    while (start <= options.size()) {
        std::size_t comma = options.find(',', start);
        if (comma == std::string::npos) {
            comma = options.size();   // the last option has no comma after it
        }
        // compare(position, length, text) == 0 means "this slice equals text".
        if (options.compare(start, comma - start, wanted) == 0) {
            return true;
        }
        start = comma + 1;
    }
    return false;
}
}  // namespace

bool parseLoadAvg(const std::string& line, LoadAvg& out) {
    std::istringstream iss(line);
    LoadAvg parsed;   // filled locally; copied to "out" only if the line was valid
    iss >> parsed.one >> parsed.five >> parsed.fifteen;
    if (iss.fail()) {
        return false;
    }
    if (parsed.one < 0.0 || parsed.five < 0.0 || parsed.fifteen < 0.0) {
        return false;   // a load average can't be negative
    }
    out = parsed;
    return true;
}

LoadAvg readLoadAvg() {
    LoadAvg load;
    std::ifstream file("/proc/loadavg");
    std::string line;
    if (std::getline(file, line)) {
        parseLoadAvg(line, load);   // on failure "load" simply stays zero
    }
    return load;
}

double readUptimeSeconds() {
    std::ifstream file("/proc/uptime");   // "12345.67 98765.43": uptime, then idle time
    double seconds = 0.0;
    file >> seconds;                       // if the read fails, "seconds" stays 0
    return seconds;
}

std::string unescapeMountField(const std::string& field) {
    std::string out;
    for (std::size_t i = 0; i < field.size(); ++i) {
        // An escape is a backslash followed by exactly three octal digits (0-7).
        // "i + 3 < size" makes sure those three characters exist.
        if (field[i] == '\\' && i + 3 < field.size()) {
            const char a = field[i + 1];
            const char b = field[i + 2];
            const char c = field[i + 3];
            if (a >= '0' && a <= '7' && b >= '0' && b <= '7' && c >= '0' && c <= '7') {
                const int value = (a - '0') * 64 + (b - '0') * 8 + (c - '0');
                if (value <= 255) {   // three octal digits can reach 511, which is not a byte
                    out.push_back(static_cast<char>(value));
                    i += 3;           // skip the three digits (the loop's ++i skips the backslash)
                    continue;
                }
            }
        }
        out.push_back(field[i]);
    }
    return out;
}

bool isRealFilesystem(const std::string& fsType) {
    // A function-local static is built once, the first time the function runs.
    // Network filesystems (nfs, cifs) are left out on purpose: statvfs() on a
    // mount whose server is unreachable can block for a very long time, and that
    // would freeze the background thread (and make quitting hang).
    // "9p" and "drvfs" are how WSL exposes the Windows drives (/mnt/c).
    static const std::set<std::string> kReal = {
        "ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "vfat", "msdos", "exfat",
        "ntfs", "ntfs3", "fuseblk", "zfs", "jfs", "reiserfs", "9p", "drvfs"};
    return kReal.count(fsType) > 0;
}

bool parseMountLine(const std::string& line, MountInfo& out) {
    std::istringstream iss(line);
    std::string device;
    std::string mountPoint;
    std::string fsType;
    std::string options;                     // e.g. "rw,relatime" (the 4th column)
    iss >> device >> mountPoint >> fsType;   // these three are required
    if (iss.fail()) {
        return false;
    }
    iss >> options;                          // a short line may lack it: then it stays empty

    // Names come from the system configuration (and can be chosen by whoever mounts something),
    // so replace control characters before they reach the screen.
    out.device = sanitizeName(unescapeMountField(device));
    out.mountPoint = sanitizeName(unescapeMountField(mountPoint));
    out.fsType = fsType;
    out.readOnly = hasOption(options, "ro");
    return true;
}

std::vector<MountInfo> readMounts() {
    std::vector<MountInfo> result;
    std::set<std::string> seenDevices;   // the same disk mounted twice (bind mounts) is shown once

    std::ifstream file("/proc/mounts");
    std::string line;
    while (std::getline(file, line) && result.size() < kMaxMounts) {
        MountInfo m;
        if (!parseMountLine(line, m) || !isRealFilesystem(m.fsType)) {
            continue;
        }
        if (seenDevices.count(m.device) > 0) {
            continue;
        }

        m.disk = readDiskInfo(m.mountPoint.c_str());
        if (m.disk.totalBytes == 0) {
            continue;   // unreadable (no permission) or empty
        }

        seenDevices.insert(m.device);   // only remember it once it worked
        result.push_back(m);
    }
    return result;
}