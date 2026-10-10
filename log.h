#pragma once
#include <iostream>

using namespace std;

enum class LogLevel { Info, Warning, Error };

// 2026-10-10 18:12:46 [WARN] CPU usage high: 91.2%
void logEvent(LogLevel level, const string& message);
