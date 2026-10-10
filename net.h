#pragma once

struct NetInfo {
    unsigned long long rxBytes = 0; // received
    unsigned long long txBytes = 0; // transmitted
};

// speed between two readings 
struct NetRates {
    double rxBytesPerSec = 0.0;
    double txBytesPerSec = 0.0;
};

NetInfo readNetInfo();
NetRates netRates(const NetInfo& before, const NetInfo& after, double seconds);