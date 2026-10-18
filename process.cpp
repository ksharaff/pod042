#include "process.h"
#include <algorithm>    // std::sort, std::all_of, std::max
#include <cctype>       // std::isdigit
#include <filesystem>   // std::filesystem: walk folders, read symlinks
#include <fstream>
#include <sstream>
#include <thread>       // std::thread::hardware_concurrency(): number of CPU cores
#include <unistd.h>     // sysconf(): ask the OS for the memory page size

// A short alias, so we write fs::path instead of std::filesystem::path.
namespace fs = std::filesystem;

// Helpers private to this file.
namespace {

// Sum of all 8 CPU counters: total "ticks" the machine has had since boot.
unsigned long long totalTicks(const cpuInfo& c) {
    return static_cast<unsigned long long>(
        c.user + c.nice + c.system + c.idle + c.iowait + c.irq + c.softirq + c.steal);
}

// True if the text is only digits. Folders in /proc named "1234" are processes;
// the others ("net", "cpuinfo", ...) are not.
bool isNumber(const std::string& s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(),
                       [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

// Reads /proc/<pid>/stat. It is ONE line like:
//   1234 (my program) S 1 1234 ... 105 42 ...
// Field 2 is the name in parentheses (it may contain spaces and even ")"),
// and fields 14 and 15 are the CPU time the process spent in user mode and
// kernel mode. Returns false if the process vanished or the line is odd.
bool readStat(int pid, std::string& name, unsigned long long& ticks) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(file, line)) {
        return false;   // the process ended while we were looking: normal, just skip it
    }

    // The name is between the FIRST "(" and the LAST ")", because names can
    // contain parentheses themselves. rfind searches from the end.
    std::size_t open = line.find('(');
    std::size_t close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return false;
    }
    name = sanitizeName(line.substr(open + 1, close - open - 1));

    // Everything after ")" is plain space-separated numbers (and the state letter).
    std::istringstream iss(line.substr(close + 1));
    std::string state;                // field 3, e.g. "S" for sleeping
    long long skip = 0;               // signed, because some fields can be -1
    unsigned long long utime = 0;     // field 14
    unsigned long long stime = 0;     // field 15

    iss >> state;
    for (int i = 0; i < 10; ++i) {    // skip fields 4..13
        iss >> skip;
    }
    iss >> utime >> stime;
    if (iss.fail()) {
        return false;
    }

    ticks = utime + stime;
    return true;
}

// Resident memory in KB from /proc/<pid>/statm: "size resident shared ...",
// all counted in memory PAGES (usually 4 KB each).
unsigned long long readRssKb(int pid, unsigned long long pageKb) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/statm");
    unsigned long long size = 0;
    unsigned long long resident = 0;
    file >> size >> resident;   // if the read fails, both stay 0
    return resident * pageKb;
}

// Process name from /proc/<pid>/comm (a file with just the name on one line).
std::string readComm(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(file, name);
    return sanitizeName(name);   // empty if we couldn't read it
}

}  // namespace

std::vector<ProcessInfo> ProcessSampler::sample(const cpuInfo& cpuNow) {
    std::vector<ProcessInfo> result;

    const unsigned long long totalNow = totalTicks(cpuNow);

    // How many ticks passed on the whole machine since the last sample.
    // If this is the first sample, we have nothing to compare with: use 0.
    const unsigned long long totalDelta =
        (primed_ && totalNow > prevTotal_) ? totalNow - prevTotal_ : 0;

    // "top" shows 100% for one fully busy core, so multiply by the core count.
    // std::max(1u, ...) protects against hardware_concurrency() returning 0.
    const unsigned int cores = std::max(1u, std::thread::hardware_concurrency());

    // The size of one memory page in KB (normally 4).
    const unsigned long long pageKb =
        static_cast<unsigned long long>(sysconf(_SC_PAGESIZE)) / 1024;

    std::map<int, unsigned long long> nowTicks;   // becomes prevTicks_ at the end

    // Walk /proc. The versions of these functions that take an std::error_code
    // report problems through "ec" instead of throwing exceptions. Processes
    // appear and disappear while we walk, so errors are normal here.
    std::error_code ec;
    fs::directory_iterator it("/proc", ec);
    fs::directory_iterator end;   // a default-constructed iterator means "past the end"

    for (; !ec && it != end; it.increment(ec)) {
        std::string folder = it->path().filename().string();   // e.g. "1234"
        if (!isNumber(folder)) {
            continue;   // not a process folder
        }

        int pid = 0;
        try {
            pid = std::stoi(folder);
        } catch (const std::exception&) {
            continue;
        }

        std::string name;
        unsigned long long ticks = 0;
        if (!readStat(pid, name, ticks)) {
            continue;   // vanished or unreadable
        }

        ProcessInfo p;
        p.pid = pid;
        p.name = name;
        p.rssKb = readRssKb(pid, pageKb);

        nowTicks[pid] = ticks;

        // CPU% = this process's ticks since last time, as a share of ALL ticks
        // since last time. It needs a previous sample of the SAME pid.
        auto prev = prevTicks_.find(pid);
        if (primed_ && prev != prevTicks_.end() && totalDelta > 0 && ticks >= prev->second) {
            double share = static_cast<double>(ticks - prev->second) /
                           static_cast<double>(totalDelta);
            p.cpuPercent = share * 100.0 * static_cast<double>(cores);
        }

        result.push_back(p);
    }

    // Keep this sample for next time. std::move hands over the map without copying it.
    prevTicks_ = std::move(nowTicks);
    prevTotal_ = totalNow;
    primed_ = true;

    // Busiest first; on a tie, the one using more memory first.
    std::sort(result.begin(), result.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  if (a.cpuPercent != b.cpuPercent) {
                      return a.cpuPercent > b.cpuPercent;
                  }
                  return a.rssKb > b.rssKb;
              });

    return result;
}

void ProcessSampler::reset() {
    prevTicks_.clear();
    prevTotal_ = 0;
    primed_ = false;
}

std::map<unsigned long long, SocketOwner> readSocketOwners() {
    std::map<unsigned long long, SocketOwner> owners;

    std::error_code ec;
    fs::directory_iterator procIt("/proc", ec);
    fs::directory_iterator end;

    for (; !ec && procIt != end; procIt.increment(ec)) {
        std::string folder = procIt->path().filename().string();
        if (!isNumber(folder)) {
            continue;
        }

        int pid = 0;
        try {
            pid = std::stoi(folder);
        } catch (const std::exception&) {
            continue;
        }

        // /proc/<pid>/fd holds one symlink per file the process has open.
        // A socket looks like:   /proc/1234/fd/5 -> "socket:[98765]"
        // where 98765 is the socket's inode, the same number that appears in the
        // last column of /proc/net/tcp. Other users' fd folders are not readable,
        // so opening it fails and we skip that process.
        std::error_code fdEc;
        fs::directory_iterator fdIt(procIt->path() / "fd", fdEc);   // "/" joins path parts
        if (fdEc) {
            continue;
        }

        std::string name = readComm(pid);

        for (; !fdEc && fdIt != end; fdIt.increment(fdEc)) {
            std::error_code linkEc;
            std::string target = fs::read_symlink(fdIt->path(), linkEc).string();
            if (linkEc) {
                continue;
            }

            // rfind(text, 0) == 0 is the trick for "starts with".
            if (target.rfind("socket:[", 0) != 0) {
                continue;
            }

            try {
                // substr(8) drops "socket:[". stoull stops at the closing "]".
                unsigned long long inode = std::stoull(target.substr(8));
                // emplace only inserts if the key is new, so if a socket is shared
                // by several processes, the first one found keeps it.
                owners.emplace(inode, SocketOwner{pid, name});
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    return owners;
}

std::string sanitizeName(std::string name) {
    for (char& ch : name) {   // "char&" = a reference, so assigning to ch edits the string
        unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u == 0x7F) {   // control characters
            ch = '?';
        }
    }
    return name;   // bytes >= 0x80 stay, so non-English names still work
}