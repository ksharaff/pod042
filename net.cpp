#include "net.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

using namespace std;

NetInfo readNetInfo() {
    NetInfo info;
    ifstream file("/proc/net/dev");
    string line;

    getline(file, line); // skip 1st header
    getline(file, line); // skip 2nd header

    while(getline(file, line)) {
        // turn eth0:123 into eth0 123
        replace(line.begin(), line.end(), ':', ' ');
        istringstream iss(line);
        string name;
        unsigned long long rx = 0, tx = 0, skip = 0;

        iss >> name >> rx; // interface name then received bytes
        for(int i = 0; i < 7; i++) {
            iss >> skip;
        }

        iss >> tx; // next col is transmitted bytes.

        if(name!="lo") {
            info.rxBytes += rx;
            info.txBytes += tx;
        }
    }
    return info;
}

NetRates netRates(const NetInfo& before, const NetInfo& after, double seconds) {
    NetRates r;
    if(seconds <= 0.0) {
        return r; // avoid dividing by 0
    }

    if(after.rxBytes >= before.rxBytes) {
        r.rxBytesPerSec = static_cast<double>(after.rxBytes - before.rxBytes) / seconds;
    }
    if(after.txBytes >= before.txBytes) {
        r.txBytesPerSec = static_cast<double>(after.txBytes - before.txBytes) / seconds;
    }
    return r;
}