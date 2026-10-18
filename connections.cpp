#include "connections.h"
#include <algorithm>   // std::stable_sort
#include <cstdio>      // std::snprintf
#include <fstream>
#include <sstream>
#include <stdexcept>   // std::exception (stoul throws on bad input)

// Anonymous namespace: these helpers are private to this file.
namespace {

// Turns the kernel's number into dotted text.
// On x86 the kernel prints the 4 IP bytes in little-endian order, so the FIRST
// byte of the address is the LOWEST byte of the number. 0x0100007F -> 127.0.0.1.
std::string hexToIp(unsigned long value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
                  value & 0xFF,            // & 0xFF keeps only the lowest 8 bits (one byte)
                  (value >> 8) & 0xFF,     // >> 8 shifts right by 8 bits: next byte
                  (value >> 16) & 0xFF,
                  (value >> 24) & 0xFF);
    return buf;
}

// Turns the state code from the file into a name.
// These numbers come from the Linux kernel's TCP state table.
std::string stateName(unsigned long code) {
    switch (code) {
        case 0x01: return "ESTABLISHED";
        case 0x02: return "SYN_SENT";
        case 0x03: return "SYN_RECV";
        case 0x04: return "FIN_WAIT1";
        case 0x05: return "FIN_WAIT2";
        case 0x06: return "TIME_WAIT";
        case 0x07: return "CLOSE";
        case 0x08: return "CLOSE_WAIT";
        case 0x09: return "LAST_ACK";
        case 0x0A: return "LISTEN";
        case 0x0B: return "CLOSING";
        default:   return "UNKNOWN";
    }
}

// Lower number = shown earlier in the list.
int stateRank(const std::string& state) {
    if (state == "ESTABLISHED") return 0;
    if (state == "LISTEN")      return 1;
    return 2;
}

// Splits "0100007F:1F90" into an address and a port.
// Returns false if the text isn't in that format.
bool parseEndpoint(const std::string& text, std::string& addr, unsigned int& port) {
    std::size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    try {
        // stoul(text, nullptr, 16) = "string to unsigned long, base 16 (hex)".
        // It throws an exception on garbage, so we catch it below.
        unsigned long ip = std::stoul(text.substr(0, colon), nullptr, 16);
        unsigned long p  = std::stoul(text.substr(colon + 1), nullptr, 16);
        addr = hexToIp(ip);
        port = static_cast<unsigned int>(p);
        return true;
    } catch (const std::exception&) {
        return false;   // malformed line: the caller skips it
    }
}

}  // namespace

std::vector<Connection> readConnections() {
    std::vector<Connection> result;

    std::ifstream file("/proc/net/tcp");
    if (!file.is_open()) {
        return result;   // empty list; the UI shows zero connections
    }

    std::string line;
    std::getline(file, line);   // skip the header line

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string slot, local, remote, stateHex;
        // The first four columns: slot ("0:"), local, remote, state.
        iss >> slot >> local >> remote >> stateHex;

        Connection c;
        if (!parseEndpoint(local, c.localAddr, c.localPort)) continue;    // skip bad lines
        if (!parseEndpoint(remote, c.remoteAddr, c.remotePort)) continue;

        try {
            c.state = stateName(std::stoul(stateHex, nullptr, 16));
        } catch (const std::exception&) {
            continue;
        }
        result.push_back(c);
    }

    // stable_sort keeps the original order among equal elements, so the list
    // doesn't shuffle around on every redraw.
    // The third argument is a lambda that says "does a come before b?".
    std::stable_sort(result.begin(), result.end(),
                     [](const Connection& a, const Connection& b) {
                         return stateRank(a.state) < stateRank(b.state);
                     });
    return result;
}