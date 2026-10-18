#pragma once
#include <string>
#include <vector>

struct Connection {
    std::string localAddr; // 127.0.0.1
    unsigned int localPort = 0; 

    std::string remoteAddr; // 0.0.0.0
    unsigned int remotePort = 0;

    std::string state; // ESTABLISHED, LISTEN
    
    unsigned long long inode = 0;   // socket id; used to find the owning process
};
// 0x0100007F -> "127.0.0.1" (the kernel prints the 4 IP bytes in little-endian order).
std::string hexToIp(unsigned long value);

// "00000000000000000000000001000000" -> "::1". Returns "" if the text isn't 32 hex digits.
std::string hexToIp6(const std::string& hex);

// 0x01 -> "ESTABLISHED", 0x0A -> "LISTEN", anything unknown -> "UNKNOWN".
std::string stateName(unsigned long code);

// Splits "0100007F:1F90" into "127.0.0.1" and 8080. Returns false for malformed text.
bool parseEndpoint(const std::string& text, std::string& addr, unsigned int& port);

// "127.0.0.1" + 8080 -> "127.0.0.1:8080", and "::1" + 8080 -> "[::1]:8080".
// IPv6 addresses contain ':' themselves, so they are wrapped in brackets.
std::string endpoint(const std::string& addr, unsigned int port);

std::vector<Connection> readConnections();
