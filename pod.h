#pragma once
#include <string>

// 3725.0 -> "01:02:05" (hours:minutes:seconds). Negative input gives "00:00:00".
std::string formatDuration(double seconds);

// The Pod's status report, written in its own voice.
//   cpuPercent, memPercent: 0.0 - 100.0
//   uptimeSeconds:          how long POD-042 has been running
// Picks the message by severity: high CPU first, then low memory, otherwise nominal.
std::string podHeartbeat(double cpuPercent, double memPercent, double uptimeSeconds);