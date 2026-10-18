#pragma once
#include <string>
#include <vector>

struct Connection {
    std::string localAddr; // 127.0.0.1
    unsigned int localPort = 0; 

    std::string remoteAddr; // 0.0.0.0
    unsigned int remotePort = 0;

    std::string state; // ESTABLISHED, LISTEN
};

std::vector<Connection> readConnections();