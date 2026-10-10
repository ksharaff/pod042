#include "log.h"
#include <ctime> // time(), localtime_r()
#include <fstream>
#include <iomanip> // std::put_time for data/time

using namespace std;

// similar to private static in Java:
namespace {
    const char* levelName(LogLevel level) {
        switch(level) {
            case LogLevel::Info:    return "INFO";
            case LogLevel::Warning: return   "WARN";
            case LogLevel::Error: return "ERROR";
        }
        return " ";
    }
}

void logEvent(LogLevel level, const string& message) {
    ofstream file("pod042.log", std::ios::app); // std::ios:app means append to end instead of overwriting file
    if(!file.is_open()){
        return; // cant log
    }

    time_t now = time(nullptr); 
    tm local{}; // (year, month, ...)
    localtime_r(&now, &local); // fill local in my time zone

    file << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
         << " [" << levelName(level) << "] "
         << message << "\n";
}
