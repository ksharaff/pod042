// MOST COMMENTS ARE WRITTEN BY THE AUTHOR HIMSELF
// THE GOAT
// KHALED SHARAFEDDIN

// some are written by the second goat, CLAUDE

// ---- Standard library headers ----
#include <atomic>    // std::atomic: a variable that is safe to share between threads
#include <chrono>    // time utilities (durations, clocks)
#include <cstdio>    // std::snprintf: formats numbers into text
#include <fstream>   // std::ifstream: reads files (used to show the log)
#include <mutex>     // std::mutex: a lock that stops two threads touching data at once
#include <string>
#include <thread>    // std::thread: runs a function in the background
#include <vector>
#include <iostream>
#include <cctype>   // std::toupper: converts a letter to uppercase
#include <map>
#include <set>


// FTXUI headers
// component.hpp:          interactive pieces (menus, containers, key handling)
// screen_interactive.hpp: the fullscreen app that runs the event loop
// elements.hpp:           things you draw (text, gauge, borders, vbox, hbox, ...)
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "chips.h"
#include "cpu.h"
#include "disk.h"
#include "loadout.h"
#include "log.h"
#include "meminfo.h"
#include "net.h"
#include "profile.h"
#include "connections.h"
#include "process.h"
#include "snapshot.h"
#include "pod.h"

using namespace ftxui;

// warning color
const Color kAmber = Color::RGB(255, 176, 0);

const double kWarnFraction = 0.8; // gauge turns into color at or above this number
const std::size_t kMaxProcessRows = 12; // How many processes the screen shows (also limits what we copy around each second).

const int kTabChips = 3; // loadout presets

const int kHeartbeatSeconds = 60;

// Typewriter timing for the boot sequence. Raise them to type slower.
const int kCharDelayMs = 30;
const int kLinePauseMs = 200;   



struct Telemetry {
    MemInfo mem;
    DiskInfo disk;
    double cpuPercent = 0.0;
    NetRates net;
    std::vector<ProcessInfo> processes; // top processes by CPU (empty unless the chip is installed)
    std::size_t processCount = 0;
};

// formats 12.345 to 12.3%
std::string pct(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", v);
    return buf;
}

// format Bytes per second to KB/s
std::string kbps(double bytesPerSec) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f KB/s", bytesPerSec / 1024.0);
    return buf;
}

// Removes spaces, tabs and newlines from both ends of a string.
std::string trim(const std::string& s) {
    const char* whitespace = " \t\r\n";
    std::size_t start = s.find_first_not_of(whitespace); //npos = "not found"
    if (start == std::string::npos) {
        return "";
    }
    std::size_t end = s.find_last_not_of(whitespace);
    return s.substr(start, end - start + 1);   // substr(startPosition, length)
}


std::size_t nextCharEnd(const std::string& s, std::size_t pos) {
    ++pos;   // always step over the first byte
    while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        ++pos;
    }
    return pos;
}


std::string toUpper(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// "127.0.0.1" + 8080 -> "127.0.0.1:8080"
std::string endpoint(const std::string& addr, unsigned int port) {
    if (addr.find(':') != std::string::npos) {
        return "[" + addr + "]:" + std::to_string(port);
    }
    return addr + ":" + std::to_string(port);
}

// Formats one table row with fixed-width columns.
// "%-12s" means: print a string, left-aligned, padded with spaces to 12 characters.
// The terminal uses a monospaced font, so padded text lines up as a table.
// .c_str() gives the C-style text that snprintf's %s expects.
std::string formatRow(const std::string& state, const std::string& local,
                      const std::string& remote, const std::string& owner) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%-12s %-30s %-30s %s",
                  state.c_str(), local.c_str(), remote.c_str(), owner.c_str());
    return buf;
}

// One row of the process table, with fixed-width columns.
// "%-8s" = text, left-aligned, 8 wide. "%8s" = right-aligned, so numbers line up.
std::string formatProcessRow(const std::string& pid, const std::string& name,
                             const std::string& cpu, const std::string& mem) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%-8s %-18s %8s %10s",
                  pid.c_str(), name.c_str(), cpu.c_str(), mem.c_str());
    return buf;
}
// ----------------------------------------------------------------------------
// meter(): draws one row like   CPU   [#######     ]  42.0%
//
// In FTXUI, everything you draw is an "Element". You build bigger Elements out
// of smaller ones:
//   text("hi")          a line of text
//   gauge(0.5f)         a horizontal progress bar (0.0 = empty, 1.0 = full)
//   hbox({a, b, c})     puts elements side by side (horizontal box)
//   vbox({a, b, c})     stacks elements top to bottom (vertical box)
//
// The "|" operator applies a "decorator" that changes how an element looks or
// is sized, for example:
//   | bold                       makes text bold
//   | border                     draws a box around it
//   | size(WIDTH, EQUAL, 6)      forces the width to exactly 6 characters
//   | flex                       lets it stretch to fill leftover space
// ----------------------------------------------------------------------------

// One row: label, horizontal gauge, and value on the right
// CPU [######      ] 42.0%
Element meter(const std::string& label, double fraction, const std::string& value) {
    Element bar = gauge(static_cast<float>(fraction));

    if(fraction >= kWarnFraction) {
        bar = bar | color(kAmber);
    }
    
    return hbox({
        text(label) | size(WIDTH, EQUAL, 6), // width exactly 6 characters
        gauge(static_cast<float>(fraction)) | flex, // flex to fill leftover space
        text(" " + value),
    });
}

// Builds the SYSTEM STATUS screen from a snapshot
Element statusPanel(const Telemetry& t, bool diskOn, bool netOn){
    // constexpr = constant computed at compile time.
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;

    double memUsedKb = static_cast<double>(t.mem.totalKb - t.mem.availableKb);
    double memFrac = t.mem.totalKb > 0 ? memUsedKb / static_cast<double>(t.mem.totalKb) : 0.0;

    Element diskRow;
    if (diskOn) {
        double diskUsed = static_cast<double>(t.disk.totalBytes - t.disk.availableBytes);
        double diskFrac = t.disk.totalBytes > 0 ? diskUsed / static_cast<double>(t.disk.totalBytes) : 0.0;
        std::string diskText = t.disk.totalBytes > 0
            ? std::to_string(static_cast<long>(diskUsed / GB)) + " / " +
              std::to_string(static_cast<long>(static_cast<double>(t.disk.totalBytes) / GB)) + " GB"
            : "READING...";
        diskRow = meter("DISK", diskFrac, diskText);
    } else {
        diskRow = text("DISK    [" + kChipDisk + " NOT INSTALLED]") | dim;
    }

    Elements netRows;
    if (netOn) {
        netRows.push_back(text("NETWORK"));
        netRows.push_back(text("  DOWNLOAD:  " + kbps(t.net.rxBytesPerSec)));
        netRows.push_back(text("  UPLOAD:    " + kbps(t.net.txBytesPerSec)));
    } else {
        netRows.push_back(text("NETWORK [" + kChipNetwork + " NOT INSTALLED]") | dim);
    }

    return vbox({
        text("SYSTEM STATUS") | bold,
        separator(),
        meter("CPU", t.cpuPercent / 100.0, pct(t.cpuPercent)),
        separatorEmpty(),
        meter("MEM", memFrac,
              std::to_string(static_cast<long>(memUsedKb / 1024.0)) + " / " +
              std::to_string(t.mem.totalKb / 1024) + " MB"),
        separatorEmpty(),

        diskRow,
        separator(),
        vbox(std::move(netRows)),
    });
}

// std::size_t is unsigned integer 64-bit
// std::size_t n = lines.size(); // .size() returns size_t not int
// lines[0]; // index in [] is also size_t

// std::ptrdiff_t is a signed integer 64-bit
// it holds the diff between two positions in memory.

// returns last count lines of a text file
// used by system logs screen to show recent events.
std::vector<std::string> readLogTail(const std::string& path, std::size_t count) {
    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while(std::getline(file, line)) {
        lines.push_back(line);
    }
    // if there are more lines than what we want, erase oldest ones from front
    if(lines.size() > count) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(count));
    }
    return lines;
}

// =====================================================
// MAIN
// =====================================================

int main(int argc, char** argv) {
    // --json: print ONE snapshot as JSON and exit. No prompt, no UI, so other
    // programs (scripts, a future web frontend) can read the output.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            ChipManager chips(4);
            if (!loadLoadout(chips, "loadout.json")) {
                applyDefaultLoadout(chips);   // defined in Part C below
            }
            std::cout << takeSnapshotJson(chips) << "\n";
            return 0;
        }
    }

    logEvent(LogLevel::Info, "POD-042 Booting");

    // "./build/pod042 --rename" forces the name prompt again.
    bool askForName = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--rename") {
            askForName = true;
        }
    }

    std::string userName;
    if (askForName || !loadName(userName, "profile.json")) {
        std::cout << "POD 042: ENTER OPERATOR DESIGNATION: ";
        std::getline(std::cin, userName);
        userName = trim(userName);
        if (userName.empty()) {
            userName = "UNIT";
        }
        saveName(userName, "profile.json");
    }

    // =====================================================
    // 1. CHIPS: create manager and restore saved loadout.
    // =====================================================

    ChipManager chips(4);

    // try to restore the saved loadout; on the first run there's no file yet.
    if (!loadLoadout(chips, "loadout.json")) {
        applyDefaultLoadout(chips);   // first run: no saved loadout yet
    }

    std::vector<std::string> chipLabels; // text of each menu line
    int chipSelected = 0; // index of highlighted chip
    std::string chipMessage; // feedback line eg not enough slots

    // The UI thread WRITES them (inside updateLabels); the refresher thread only READS them.
    std::atomic<bool> netEnabled{false};
    std::atomic<bool> diskEnabled{false};
    std::atomic<bool> logEnabled{false};
    std::atomic<bool> processEnabled{false};

    // lambda: small unnamed function stored  in a variable
    // [&] = capture: the lambda can use the variables around 
    // it by reference, not by copy
    // this one rebuilds menu text from the chip manager's current state
    // so ON and OFF is always up to date

    auto updateLabels = [&] {
        chipLabels.clear();
        for(const Chip& c : chips.chips()) {
            chipLabels.push_back((c.enabled ? "[ON] " : "[OFF] ") + c.name + 
                                " (cost " + std::to_string(c.cost) + ")"); 
        }
        netEnabled  = chips.isEnabled(kChipNetwork);
        diskEnabled = chips.isEnabled(kChipDisk);
        logEnabled  = chips.isEnabled(kChipLog);
        processEnabled = chips.isEnabled(kChipProcess);
    };

    updateLabels(); // fill labels once before UI starts

    // MenuOption lets us customize menu
    MenuOption chipOption = MenuOption::Vertical();
    chipOption.on_enter = [&] {
        std::string name = chips.chips()[static_cast<std::size_t>(chipSelected)].name;
        bool wasEnabled = chips.isEnabled(name);

        // Toggle it: remove if installed, install if not.
        ChipResult result = wasEnabled ? chips.disable(name) : chips.enable(name);
        // if install refused, tell user, else nothing
        chipMessage = (result == ChipResult::NotEnoughSlots) ? "NOT ENOUGH SLOTS" : "";

        updateLabels();
        saveLoadout(chips, "loadout.json");

    };

        // Switches to the preset at "index" (0 = first preset), then saves the result.
    auto selectPreset = [&](std::size_t index) {
        const std::vector<Preset>& all = presets();
        if (index >= all.size()) {
            return;   // a key like "7" when only 3 presets exist: ignore it
        }
        applyPreset(chips, all[index].name);
        chipMessage = "LOADOUT: " + all[index].name;
        updateLabels();                      // refreshes the menu text and the chip flags
        saveLoadout(chips, "loadout.json");
    };

    // =====================================================
    // 2. TELEMETRY
    // =====================================================
    // Two threads, use mutex for locking.
    std::mutex telemtryMutex;
    Telemetry telemetry;
    telemetry.mem = readMemInfo();
    if(diskEnabled) {
        telemetry.disk=readDiskInfo("/");
    }

    // a small helper that returns COPY of shared telemetry
    // lock is held only while copying, then released at closing brace
    // several places need snapshot, so this avoids repeating the locking code.
    auto snapshotTelemetry = [&] {
        std::lock_guard<std::mutex> lock(telemtryMutex);
        return telemetry;
    };

    auto screen = ScreenInteractive::Fullscreen();

    // bool safe to read in one thread and wrie in another without mutex
    // refresher checks it to know when to stop
    std::atomic<bool> running{true};

    std::thread refresher([&] {
        cpuInfo prevCpu = readCpuInfo();
        NetInfo prevNet = readNetInfo();
        auto prevTime = std::chrono::steady_clock::now();
        ProcessSampler processSampler;   // keeps its own previous sample
        std::set<int> alertedPids;       // processes currently flagged as high CPU

        // remembers whether cpu alert is currently active
        // so we log only when the state changes
        // without this a busy cpu would add a new log line every second
        bool cpuAlertActive = false;

        bool netPrimed = false;

        // For the Pod heartbeat
        const auto startTime = std::chrono::steady_clock::now();
        auto lastHeartbeat = startTime;

        while(running) {
            // sleep 1 second
            for(int i = 0; i < 10 && running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(!running) break;

            cpuInfo nowCpu = readCpuInfo();
            auto nowTime = std::chrono::steady_clock::now();
            double seconds = std::chrono::duration<double>(nowTime - prevTime).count();

            Telemetry t;
            t.mem  = readMemInfo();
            t.cpuPercent = cpuUsagePercent(prevCpu, nowCpu);

            if(diskEnabled) {
                t.disk = readDiskInfo("/");
            }

            // NETWORK WATCH
            if(netEnabled) {
                NetInfo nowNet = readNetInfo();
                if(netPrimed) {
                    t.net = netRates(prevNet, nowNet, seconds);
                }
                prevNet = nowNet;
                netPrimed = true;
            } else {
                netPrimed = false;
            }

            // PROCESS WATCH: sample all processes, but only while the chip is installed.
            if (processEnabled) {
                std::vector<ProcessInfo> procs = processSampler.sample(nowCpu);
                t.processCount = procs.size();

                // Log a warning when a process CROSSES the threshold (not every second):
                // same edge-detection idea as the system CPU alert, but per process.
                std::set<int> stillHigh;
                for (const ProcessInfo& p : procs) {
                    if (p.cpuPercent > 80.0) {
                        stillHigh.insert(p.pid);
                        if (alertedPids.count(p.pid) == 0) {   // count() is 0 if not in the set
                            logEvent(LogLevel::Warning,
                                     "Process high CPU: " + p.name + " (pid " +
                                     std::to_string(p.pid) + ") " + pct(p.cpuPercent));
                        }
                    }
                }
                alertedPids = stillHigh;   // processes that dropped below are forgotten

                // Keep only the top rows. The list is sorted, so these are the busiest.
                if (procs.size() > kMaxProcessRows) {
                    procs.resize(kMaxProcessRows);
                }
                t.processes = std::move(procs);
            } else {
                processSampler.reset();   // chip removed: forget the old sample
                alertedPids.clear();
            }

            bool cpuHigh = t.cpuPercent > 80.0;
            if(cpuHigh && !cpuAlertActive) {
                logEvent(LogLevel::Warning, "CPU usage high: " + pct(t.cpuPercent));
            } else if(!cpuHigh && cpuAlertActive) {
                logEvent(LogLevel::Info, "CPU usage back to normal");
            }
            cpuAlertActive = cpuHigh;

            // Pod heartbeat: once every kHeartbeatSeconds, one line in the log.
            if (nowTime - lastHeartbeat >= std::chrono::seconds(kHeartbeatSeconds)) {
                double memPercent = t.mem.totalKb > 0
                    ? 100.0 * static_cast<double>(t.mem.totalKb - t.mem.availableKb) /
                          static_cast<double>(t.mem.totalKb)
                    : 0.0;
                double uptime = std::chrono::duration<double>(nowTime - startTime).count();
                logEvent(LogLevel::Info, podHeartbeat(t.cpuPercent, memPercent, uptime));
                lastHeartbeat = nowTime;
            }

            {
                std::lock_guard<std::mutex> lock(telemtryMutex);
                telemetry = t;
            }

            prevCpu = nowCpu;
            prevTime  = nowTime;

            screen.PostEvent(Event::Custom);
        }
    });
    // =====================================================
    // 3. BOOT SEQUENCE
    // =====================================================

    const std::vector<std::string> bootLines = {
        "POD 042: INITIALIZING ANDROID SYSTEMS...",
        "MEMORY BANKS ............ OK",
        "PROCESSOR CORES ......... OK",
        "STORAGE ARRAY ........... " + std::string(chips.isEnabled(kChipDisk) ? "OK" : "OFFLINE"),
        "NETWORK INTERFACE ....... " + std::string(chips.isEnabled(kChipNetwork) ? "OK" : "OFFLINE"),
        "CHIP LOADOUT ............ " + std::to_string(chips.usedSlots()) + "/" +
            std::to_string(chips.totalSlots()) + " SLOTS IN USE",
        "ALL SYSTEMS NOMINAL. WELCOME, " + toUpper(userName) + ".",
    };

        std::atomic<bool> booting{true};

    
    std::atomic<std::size_t> bootChars{0};

    // Total size of the boot text, used for the progress bar.
    std::size_t totalBootChars = 0;
    for (const std::string& line : bootLines) {
        totalBootChars += line.size();
    }

    // Sleeps "ms" milliseconds in 10 ms slices; wakes early on skip or quit.
    // Slices must be shorter than the per-character delay, or the typing speed
    // would be rounded up to the slice length.
    auto nap = [&](int ms) {
        for (int waited = 0; waited < ms && booting && running; waited += 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };

    // The boot thread "types" the text one character at a time.
    std::thread bootThread([&] {
        std::size_t revealed = 0;   // bytes shown so far

        for (const std::string& line : bootLines) {
            std::size_t pos = 0;    // position inside this line
            while (pos < line.size() && booting && running) {
                std::size_t next = nextCharEnd(line, pos);   // one whole character
                revealed += next - pos;
                pos = next;

                bootChars = revealed;
                screen.PostEvent(Event::Custom);   // redraw
                nap(kCharDelayMs);
            }
            nap(kLinePauseMs);   // short pause once a line is complete
        }

        nap(700);              // pause on the finished screen
        booting = false;       // switch to the main UI
        screen.PostEvent(Event::Custom);
    });

    auto bootScreen = [&]() -> Element {
        const std::size_t shown = bootChars;   // read the atomic once, then reuse it

        Elements rows;
        rows.push_back(text("YoRHa // BOOT SEQUENCE") | bold);
        rows.push_back(separator());

        // Walk the lines, spending the revealed characters on each one:
        //   enough left for the whole line -> draw it, subtract its length
        //   not enough                      -> draw the first part plus a cursor, stop
        std::size_t remaining = shown;
        for (const std::string& line : bootLines) {
            if (remaining == 0) {
                break;   // nothing left to show for this or any later line
            }
            if (remaining >= line.size()) {
                rows.push_back(text(line));
                remaining -= line.size();
            } else {
                // "█" is a block cursor at the point where typing currently is.
                rows.push_back(text(line.substr(0, remaining) + "█"));
                remaining = 0;
            }
        }
        if (shown == 0) {
            rows.push_back(text("█"));   // before the first character, show only the cursor
        }

        rows.push_back(separator());
        rows.push_back(gauge(static_cast<float>(shown) / static_cast<float>(totalBootChars)));
        rows.push_back(text("PRESS ANY KEY TO SKIP") | dim);

        return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, 56) | center;
    };

    // =====================================================
    // 4. UI
    // =====================================================
    // Element: something you draw
    // Component: something that draws and reacts to input
    // Renderer() turns a function that returns an element into a component

    // Left menu: list of screens. "tab" holds the selected index
    // menu stores the address of "tab" and updates it as you move
    std::vector<std::string> tabNames = {"SYSTEM STATUS", "NETWORK CONNECTIONS",
                                         "PROCESS WATCH", "CHIP CONFIGURATION",
                                         "SYSTEM LOGS"};
    int tab = 0;
    Component tabMenu = Menu(&tabNames, &tab, MenuOption::Vertical());

    // Screen 1: status
    // This lambda runs every time the screen is redrawn
    Component statusView = Renderer([&] {
        return statusPanel(snapshotTelemetry(), diskEnabled, netEnabled);
    });

    // Screen: network connections. Only works if the NETWORK SCAN chip is installed.
    // It reads /proc/net/tcp directly each time it's drawn. That's cheap, and it
    // keeps this screen free of shared state, so no mutex is needed.
    Component connectionsView = Renderer([&]() -> Element {
        if (!netEnabled) {
            return vbox({
                text("NETWORK CONNECTIONS") | bold,
                separator(),
                text(kChipNetwork + " CHIP NOT INSTALLED") | color(kAmber),
                text("Install it on the CHIP CONFIGURATION screen.") | dim,
            });
        }

        std::vector<Connection> conns = readConnections();

        // Which process owns each socket? Only looked up if PROCESS WATCH is installed.
        std::map<unsigned long long, SocketOwner> owners;
        if (processEnabled) {
            owners = readSocketOwners();
        }

        // Count a few states for the summary line.
        std::size_t established = 0;
        std::size_t listening = 0;
        for (const Connection& c : conns) {
            if (c.state == "ESTABLISHED") ++established;
            if (c.state == "LISTEN")      ++listening;
        }

        Elements rows;
        rows.push_back(text("NETWORK CONNECTIONS") | bold);
        rows.push_back(separator());
        rows.push_back(text("ESTABLISHED: " + std::to_string(established) +
                            "   LISTENING: " + std::to_string(listening) +
                            "   TOTAL: " + std::to_string(conns.size())));
        rows.push_back(separator());
        rows.push_back(text(formatRow("STATE", "LOCAL", "REMOTE", "OWNER")) | dim);
        const std::size_t maxRows = 15;   // keep the table inside the window
        std::size_t shown = 0;
        for (const Connection& c : conns) {
            if (shown >= maxRows) break;

            // "-" = no owner info (chip off, or the socket has no owner such as TIME_WAIT).
            // "?" = it has an owner we are not allowed to inspect.
            std::string owner = "-";
            if (processEnabled && c.inode != 0) {
                auto found = owners.find(c.inode);
                owner = (found != owners.end())
                    ? found->second.name + " (" + std::to_string(found->second.pid) + ")"
                    : "?";
            }

            Element row = text(formatRow(c.state,
                                         endpoint(c.localAddr, c.localPort),
                                         endpoint(c.remoteAddr, c.remotePort),
                                         owner));

            // A LISTEN socket bound to 0.0.0.0 accepts connections on EVERY network
            // interface, so it's reachable from outside this machine. That deserves
            // a visual flag. A socket bound to 127.0.0.1 only accepts local traffic.
            if (c.state == "LISTEN" && (c.localAddr == "0.0.0.0" || c.localAddr == "::")) {
                row = row | color(kAmber);
            }
            rows.push_back(row);
            ++shown;
        }

        if (conns.size() > shown) {
            rows.push_back(text("... +" + std::to_string(conns.size() - shown) + " more") | dim);
        }
        rows.push_back(separator());
        rows.push_back(text("AMBER = LISTENING ON ALL INTERFACES (0.0.0.0 / ::).  ? = OWNER NOT VISIBLE WITHOUT ROOT") | dim);
        return vbox(std::move(rows));
    });

    // Screen: process watch. Only works if the PROCESS WATCH chip is installed.
    Component processesView = Renderer([&]() -> Element {
        if (!processEnabled) {
            return vbox({
                text("PROCESS WATCH") | bold,
                separator(),
                text(kChipProcess + " CHIP NOT INSTALLED") | color(kAmber),
                text("Install it on the CHIP CONFIGURATION screen.") | dim,
            });
        }

        Telemetry snap = snapshotTelemetry();

        Elements rows;
        rows.push_back(text("PROCESS WATCH") | bold);
        rows.push_back(separator());
        rows.push_back(text("PROCESSES: " + std::to_string(snap.processCount) +
                            "   SHOWING TOP " + std::to_string(snap.processes.size()) +
                            " BY CPU"));
        rows.push_back(separator());
        rows.push_back(text(formatProcessRow("PID", "NAME", "CPU", "MEMORY")) | dim);

        if (snap.processes.empty()) {
            rows.push_back(text("READING...") | dim);   // the first sample arrives within a second
        }

        for (const ProcessInfo& p : snap.processes) {
            Element row = text(formatProcessRow(std::to_string(p.pid), p.name,
                                                pct(p.cpuPercent),
                                                std::to_string(p.rssKb / 1024) + " MB"));
            if (p.cpuPercent > 80.0) {
                row = row | color(kAmber);   // same threshold as the log warning
            }
            rows.push_back(row);
        }

        rows.push_back(separator());
        rows.push_back(text("AMBER = CPU ABOVE 80% (ALSO LOGGED). 100% = ONE FULL CORE.") | dim);
        return vbox(std::move(rows));
    });

    // Screen 2: chips
    // Remderer(child, fn): child receives key presses; fn decides how everything is drawm
    // Inside fn, chipMenu->Render() draws the list itself.
    Component chipMenu = Menu(&chipLabels, &chipSelected, chipOption);
    Component chipsView = Renderer(chipMenu, [&] {
        double used = chips.usedSlots();
        double total = chips.totalSlots();
        const Chip& selected = chips.chips()[static_cast<std::size_t>(chipSelected)];

        // Build "PRESETS:  [1] DIAGNOSTIC  [2] EXPLORATION  [3] SECURITY" from the
        // preset list, so adding a preset in loadout.cpp updates the screen too.
        std::string presetLine = "PRESETS:";
        const std::vector<Preset>& all = presets();
        for (std::size_t i = 0; i < all.size(); ++i) {
            presetLine += "  [" + std::to_string(i + 1) + "] " + all[i].name;
        }

        return vbox({
            text("CHIP CONFIGURATION") | bold,
            separator(),
            meter("SLOTS", total > 0 ? used / total : 0.0,
                  std::to_string(chips.usedSlots()) + "/" + std::to_string(chips.totalSlots())),
            separator(),
            chipMenu->Render(),
            separator(),
            text(selected.description) | dim,
            text(chipMessage) | bold | color(kAmber),
            separator(),
            text(presetLine),
            text("RIGHT: focus list   ENTER: install / remove   1-3: load preset") | dim,
        });
    });

    // Screen 3: the last 20 lines of the event log, re-read on every redraw
    Component logsView = Renderer([&] {
        Elements rows;
        rows.push_back(text("System LOGS") | bold);
        rows.push_back(separator());
        for(const std::string& line : readLogTail("pod042.log", 20)) {
            Element row = text(line);
            if(line.find("[WARN]") != std::string::npos) {
                row = row | color(kAmber);
            } else if(line.find("[ERROR]") != std::string::npos) {
                row = row | color(Color::Red);
            }
            rows.push_back(text(line));
        }
        return vbox(std::move(rows));
    });
    // Container::Tab holds several components but shows only the one at index
    // "tab" (the left menu's selection).
    Component tabs = Container::Tab(
        {statusView, connectionsView, processesView, chipsView, logsView}, &tab);

    // Container::Horizontal groups the left menu and the right panel, so
    // Left/Right arrow keys move focus between them.
    Component layout = Container::Horizontal({tabMenu, tabs});

    Component root = Renderer(layout, [&]() -> Element {
        if(booting) {
            return bootScreen();
        }

        Telemetry snap = snapshotTelemetry();
        Element statusLine = snap.cpuPercent > 80.0
            ? text("POD 042: WARNING. CPU LOAD EXCEEDS THRESHOLD.") | bold | color(kAmber)
            : text("SYSTEM STATUS: OPERATIONAL") | bold;
        
        
        return vbox({
            // Header box
            vbox({
                text("YoRHa // UNIT DIAGNOSTICS") | bold,
                text("NAME: " + userName) | bold,
                statusLine,
            }) | border,
            // Middle: menu on the left, active screen on the right
            hbox({
                tabMenu->Render() | border | size(WIDTH, EQUAL, 28),
                tabs->Render() | border | flex, 
            }) | flex,
            // Footer hint
            text(" UP/DOWN: select  |  LEFT/RIGHT: switch focus  |  Q: quit ") | dim,
        });
    });

    root = CatchEvent(root, [&](Event event){
        if(booting) {
            if(event==Event::Custom || event.is_mouse()){
                return false;
            }
            booting = false;
            return true;
        }

        // On the chip screen, keys 1..9 load the matching preset.
        if (tab == kTabChips && event.is_character()) {
            char key = event.character()[0];
            if (key >= '1' && key <= '9') {
                selectPreset(static_cast<std::size_t>(key - '1'));   // '1' -> 0, '2' -> 1, ...
                return true;   // handled: the key does nothing else
            }
        }
        
        if(event==Event::Character('q') || event==Event::Character('Q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    // 5. RUN: blocks here, drawing and handling keys, until Q is pressed
    screen.Loop(root);

    // 6. SHUTDOWN: stop background thread cleanly
    running = false; // tell refresher loop to stop
    booting = false;
    refresher.join(); // wait until it has actually finished
    bootThread.join();
    logEvent(LogLevel::Info, "POD-042 Shutdown.");
    return 0;
}