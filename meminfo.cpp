#include "meminfo.h"   // quotes = your own header, <> = system/library header
#include <fstream>
#include <sstream>
#include <string>

using namespace std;

// Definition: the real implementation lives here.
MemInfo readMemInfo() {
    MemInfo info;
    ifstream file("/proc/meminfo");
    string line;
    while (getline(file, line)) {
        istringstream iss(line);
        string key;
        long value;
        iss >> key >> value;
        if (key == "MemTotal:")     info.totalKb = value;
        if (key == "MemAvailable:") info.availableKb = value;
    }
    return info;
}