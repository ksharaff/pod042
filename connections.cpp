#include "connections.h"
#include <arpa/inet.h>   // inet_ntop(), INET6_ADDRSTRLEN
#include <algorithm>     // std::stable_sort
#include <cstdio>        // std::snprintf
#include <fstream>
#include <sstream>
#include <stdexcept>     // std::exception (stoul throws on bad input)

// Helpers private to this file.
namespace {

// Lower number = shown earlier in the list.
int stateRank(const std::string& state) {
    if (state == "ESTABLISHED") return 0;
    if (state == "LISTEN")      return 1;
    return 2;
}

// Parses ALL of "text" as a hex number. Plain std::stoul silently accepts
// leftovers (it turns "12ZZ" into 0x12); this one rejects them.
// The number comes back through "out" (an output parameter); returns false on bad input.
bool parseHex(const std::string& text, unsigned long& out) {
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t used = 0;                  // how many characters stoul consumed
        out = std::stoul(text, &used, 16);
        return used == text.size();            // everything must have been consumed
    } catch (const std::exception&) {
        return false;
    }
}

// Reads one /proc/net/tcp-style file and appends its connections to "out".
// /proc/net/tcp holds IPv4 sockets and /proc/net/tcp6 holds IPv6 sockets; the
// columns are the same, only the address format differs.
void readTcpFile(const char* path, std::vector<Connection>& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;   // e.g. a system with IPv6 switched off has no tcp6 file
    }

    std::string line;
    std::getline(file, line);   // skip the header line

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string slot, local, remote, stateHex;
        std::string queues, timer, retransmits, uid, timeout;   // columns we skip
        unsigned long long inode = 0;                            // the last column we want
        iss >> slot >> local >> remote >> stateHex
            >> queues >> timer >> retransmits >> uid >> timeout >> inode;

        Connection c;
        c.inode = inode;
        if (!parseEndpoint(local, c.localAddr, c.localPort)) continue;
        if (!parseEndpoint(remote, c.remoteAddr, c.remotePort)) continue;

        unsigned long state = 0;
        if (!parseHex(stateHex, state)) continue;
        c.state = stateName(state);

        out.push_back(c);
    }
}

}  // namespace

std::string hexToIp(unsigned long value) {
    char buf[32];
    // & 0xFF keeps only the lowest 8 bits (one byte); >> 8 moves the next byte down.
    std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu.%lu",
                  value & 0xFF,
                  (value >> 8) & 0xFF,
                  (value >> 16) & 0xFF,
                  (value >> 24) & 0xFF);
    return buf;
}

std::string hexToIp6(const std::string& hex) {
    if (hex.size() != 32) {
        return "";
    }

    // 16 bytes = the whole IPv6 address, in network order.
    unsigned char bytes[16];

    // The kernel prints the address as four 32-bit words, each byte-reversed
    // exactly like the IPv4 case, so each word is unpacked lowest byte first.
    for (std::size_t w = 0; w < 4; ++w) {
        unsigned long word = 0;
        if (!parseHex(hex.substr(w * 8, 8), word)) {
            return "";
        }
        bytes[w * 4 + 0] = static_cast<unsigned char>(word & 0xFF);
        bytes[w * 4 + 1] = static_cast<unsigned char>((word >> 8) & 0xFF);
        bytes[w * 4 + 2] = static_cast<unsigned char>((word >> 16) & 0xFF);
        bytes[w * 4 + 3] = static_cast<unsigned char>((word >> 24) & 0xFF);
    }

    // inet_ntop = "network to presentation": turns raw address bytes into text,
    // including the "::" shortening. AF_INET6 says these bytes are an IPv6 address.
    char buf[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6, bytes, buf, sizeof(buf)) == nullptr) {
        return "";
    }
    return buf;
}

std::string stateName(unsigned long code) {
    // These numbers come from the Linux kernel's TCP state table.
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

bool parseEndpoint(const std::string& text, std::string& addr, unsigned int& port) {
    std::size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return false;
    }

    unsigned long portValue = 0;
    if (!parseHex(text.substr(colon + 1), portValue) || portValue > 65535) {
        return false;
    }

    // The LENGTH of the address part tells the two formats apart:
    // 8 hex digits = IPv4, 32 hex digits = IPv6.
    const std::string addrHex = text.substr(0, colon);
    std::string decoded;
    if (addrHex.size() == 8) {
        unsigned long ip = 0;
        if (!parseHex(addrHex, ip)) {
            return false;
        }
        decoded = hexToIp(ip);
    } else if (addrHex.size() == 32) {
        decoded = hexToIp6(addrHex);
        if (decoded.empty()) {
            return false;
        }
    } else {
        return false;
    }

    addr = decoded;   // only write the outputs once everything succeeded
    port = static_cast<unsigned int>(portValue);
    return true;
}

std::string endpoint(const std::string& addr, unsigned int port) {
    if (addr.find(':') != std::string::npos) {
        return "[" + addr + "]:" + std::to_string(port);
    }
    return addr + ":" + std::to_string(port);
}

std::vector<Connection> readConnections() {
    std::vector<Connection> result;
    readTcpFile("/proc/net/tcp", result);    // IPv4
    readTcpFile("/proc/net/tcp6", result);   // IPv6

    // stable_sort keeps the original order among equal elements, so the list
    // doesn't shuffle on every redraw. The lambda says "does a come before b?".
    std::stable_sort(result.begin(), result.end(),
                     [](const Connection& a, const Connection& b) {
                         return stateRank(a.state) < stateRank(b.state);
                     });
    return result;
}