// ============================================================================
// main.cpp - POD-042 terminal interface
//
//   1. A background thread ("refresher") reads system stats every second,
//      runs the event feed, and starts/stops the COMMS link.
//   2. A second background thread types out the boot sequence.
//   3. The COMMS node has its own I/O thread (see comms.cpp).
//   4. The UI (FTXUI) draws everything and reacts to keys.
// ============================================================================

#include <algorithm>    // std::min, std::any_of
#include <atomic>       // std::atomic: a variable that is safe to share between threads
#include <cctype>       // std::toupper
#include <chrono>       // time utilities
#include <cstdio>       // std::snprintf: formats numbers into text
#include <filesystem>   // std::filesystem: delete/check files
#include <fstream>      // std::ifstream / std::ofstream
#include <iostream>     // std::cin / std::cout: plain terminal input and output
#include <map>
#include <mutex>        // std::mutex: a lock so two threads don't touch data at once
#include <set>
#include <string>
#include <thread>       // std::thread: runs a function in the background
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "changes.h"
#include "chips.h"
#include "comms.h"
#include "config.h"
#include "connections.h"
#include "cpu.h"
#include "disk.h"
#include "history.h"
#include "host.h"
#include "programs.h"
#include "loadout.h"
#include "log.h"
#include "meminfo.h"
#include "net.h"
#include "pod.h"
#include "process.h"
#include "profile.h"
#include "protocol.h"
#include "snapshot.h"

using namespace ftxui;

// ----------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------

// One named color used for every warning, so the look stays consistent.
const Color kAmber = Color::RGB(255, 176, 0);

// The dark strip drawn behind every gauge, so the empty part of a bar is visible too.
const Color kTrack = Color::RGB(58, 58, 58);

// The gauge warning level (0.8 = 80%). It starts at the default and is set ONCE from
// config.json at the start of main(), before any thread is created, so every thread
// afterwards only reads it. That is why a plain (non-atomic) variable is safe here.
double gWarnFraction = 0.8;

// Files, in the folder you launch from (like the log and the loadout).
const std::string kConfigFile = "config.json";
const std::string kKnownProgramsFile = "known_programs.json";

// The most changes written to the log per sample; the rest become "... and N more".
const std::size_t kMaxChangesPerSample = 5;

// Typewriter timing for the boot sequence. Raise them to type slower.
const int kCharDelayMs = 18;    // pause after each character
const int kLinePauseMs = 200;   // extra pause after each finished line

// How many seconds of history we keep for the sparklines, and how many of them
// each sparkline shows.
const std::size_t kHistorySamples = 60;
const std::size_t kSparkWidth = 16;

// The details screen shows at most this many cores and mounts, so it fits the window.
const std::size_t kMaxCoresShown = 16;
const std::size_t kMaxMountsShown = 5;

// How many processes the process screen shows.
const std::size_t kMaxProcessRows = 12;

// Index of a screen in tabNames (used for the hotkeys). The screens are:
// STATUS 0, DETAILS 1, CONNECTIONS 2, PROCESS WATCH 3, CHIPS 4, COMMS 5, LOGS 6.
const int kTabChips = 4;
const int kTabComms = 5;

// ----------------------------------------------------------------------------
// Telemetry: one snapshot of all the numbers the screens show.
// The refresher thread builds a new one every second; the UI reads it.
// ----------------------------------------------------------------------------
struct Telemetry {
    MemInfo mem;
    DiskInfo disk;
    double cpuPercent = 0.0;
    NetRates net;
    std::vector<ProcessInfo> processes;   // top processes by CPU (empty unless PROCESS WATCH is installed)
    std::size_t processCount = 0;         // how many processes exist in total

    LoadAvg load;                         // 1 / 5 / 15 minute load average
    double uptimeSeconds = 0.0;
    std::vector<double> corePercent;      // one value per CPU core
    std::vector<MountInfo> mounts;        // only filled while DISK MONITOR is installed

    // The last kHistorySamples seconds of each value, oldest first (for the sparklines).
    std::vector<double> cpuHistory;
    std::vector<double> memHistory;
    std::vector<double> rxHistory;
    std::vector<double> txHistory;
};

// ----------------------------------------------------------------------------
// Small formatting helpers
// ----------------------------------------------------------------------------

// 12.345 -> "12.3%".  "%.1f" = one digit after the point, "%%" = a literal percent sign.
std::string pct(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", v);
    return buf;
}

// Bytes per second -> "12.3 KB/s".
std::string kbps(double bytesPerSec) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f KB/s", bytesPerSec / 1024.0);
    return buf;
}

// "%.2f": a number with 2 digits after the point, e.g. "0.52".
std::string fixed2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// Bytes -> gigabytes with one decimal, e.g. "54.2".
std::string gbText(double bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// Formats a number without trailing zeros: 80 -> "80", 55.5 -> "55.5". "%g" picks the shortest form.
std::string numText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

// Returns an uppercase copy. "s" is taken BY VALUE, so it is our own copy to change.
std::string toUpper(std::string s) {
    for (char& c : s) {   // "char&" = a reference, so assigning to c edits the string
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// A wildcard bind listens on every local interface for either address family.
bool isWildcardAddress(const std::string& address) {
    return address == "0.0.0.0" || address == "::";
}

// Returns the position just after the UTF-8 character that STARTS at "pos".
// Non-English characters take 2-4 bytes, and cutting one in half prints garbage.
// Continuation bytes always look like 10xxxxxx, which (byte & 0xC0) == 0x80 detects.
std::size_t nextCharEnd(const std::string& s, std::size_t pos) {
    ++pos;   // always step over the first byte
    while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        ++pos;
    }
    return pos;
}

// One row of the connections table, with fixed-width columns.
// "%-12s" = text, left-aligned, padded with spaces to 12 characters. The terminal font is
// monospaced, so padded text lines up as a table. .c_str() gives the C-style text %s expects.
std::string formatRow(const std::string& state, const std::string& local,
                      const std::string& remote, const std::string& owner) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%-12s %-30s %-30s %s",
                  state.c_str(), local.c_str(), remote.c_str(), owner.c_str());
    return buf;
}

// One row of the process table. "%8s" = right-aligned, so numbers line up.
std::string formatProcessRow(const std::string& pid, const std::string& name,
                             const std::string& cpu, const std::string& mem) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%-8s %-18s %8s %10s",
                  pid.c_str(), name.c_str(), cpu.c_str(), mem.c_str());
    return buf;
}

// ----------------------------------------------------------------------------
// Gauges
//
// In FTXUI everything you draw is an "Element". You build bigger ones from smaller ones:
//   text("hi")        a line of text          gauge(0.5f)    a progress bar (0.0 - 1.0)
//   hbox({a, b})      side by side            vbox({a, b})   stacked
// The "|" operator applies a "decorator" (bold, border, size, flex, color, ...).
// ----------------------------------------------------------------------------

// A gauge with the dark track behind it. It turns amber at or above the warning level,
// unless allowWarn is false (used for read-only mounts, which are expected to be full).
Element gaugeBar(double fraction, bool allowWarn = true) {
    Element bar = gauge(static_cast<float>(fraction));
    if (allowWarn && fraction >= gWarnFraction) {
        bar = bar | color(kAmber);   // decorators return a NEW element, so assign it back
    }
    // bgcolor paints the whole gauge area, so the unfilled part shows as a dark track.
    return bar | bgcolor(kTrack);
}

// Draws one row: label, gauge, optional sparkline, value.
//   spark:      a sparkline string; left out when empty
//   labelWidth: how many columns the label gets
//   barWidth:   fixed width of the bar in columns; 0 = stretch to fill the row
// A parameter written "= value" is a DEFAULT ARGUMENT: callers may leave it out.
Element meter(const std::string& label, double fraction, const std::string& value,
              const std::string& spark = "", int labelWidth = 6, int barWidth = 0) {
    Element bar = gaugeBar(fraction);

    Elements row;   // Elements = a std::vector<Element>
    row.push_back(text(label) | size(WIDTH, EQUAL, labelWidth));
    // A fixed width keeps the value right next to its bar.
    row.push_back(barWidth > 0 ? bar | size(WIDTH, EQUAL, barWidth) : bar | flex);
    if (!spark.empty()) {
        row.push_back(text(" " + spark) | dim);
    }
    row.push_back(text(" " + value));
    return hbox(std::move(row));
}

// ----------------------------------------------------------------------------
// Screens that are plain functions of a Telemetry snapshot
// ----------------------------------------------------------------------------

// The SYSTEM STATUS screen. A missing chip replaces its section with a dim "NOT INSTALLED" line.
Element statusPanel(const Telemetry& t, bool diskOn, bool netOn) {
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;

    // Memory is a core system: always shown.
    double memUsedKb = static_cast<double>(t.mem.totalKb - t.mem.availableKb);
    double memFrac = t.mem.totalKb > 0 ? memUsedKb / static_cast<double>(t.mem.totalKb) : 0.0;

    // The disk row depends on the DISK MONITOR chip.
    Element diskRow;
    if (diskOn) {
        double diskUsed = static_cast<double>(t.disk.totalBytes - t.disk.availableBytes);
        double diskFrac = t.disk.totalBytes > 0 ? diskUsed / static_cast<double>(t.disk.totalBytes) : 0.0;
        // Right after installing the chip the first reading hasn't arrived yet (total is 0).
        std::string diskText = t.disk.totalBytes > 0
            ? std::to_string(static_cast<long>(diskUsed / GB)) + " / " +
              std::to_string(static_cast<long>(static_cast<double>(t.disk.totalBytes) / GB)) + " GB"
            : "READING...";
        diskRow = meter("DISK", diskFrac, diskText);
    } else {
        diskRow = text("DISK    [" + kChipDisk + " NOT INSTALLED]") | dim;
    }

    // The network section depends on the NETWORK SCAN chip.
    Elements netRows;
    if (netOn) {
        netRows.push_back(text("NETWORK"));
        // Speeds have no natural maximum, so their sparklines scale to the largest value shown.
        netRows.push_back(hbox({
            text("  DOWN  " + kbps(t.net.rxBytesPerSec)) | size(WIDTH, EQUAL, 24),
            text(sparklineAuto(t.rxHistory, kSparkWidth)) | dim,
        }));
        netRows.push_back(hbox({
            text("  UP    " + kbps(t.net.txBytesPerSec)) | size(WIDTH, EQUAL, 24),
            text(sparklineAuto(t.txHistory, kSparkWidth)) | dim,
        }));
    } else {
        netRows.push_back(text("NETWORK [" + kChipNetwork + " NOT INSTALLED]") | dim);
    }

    return vbox({
        text("SYSTEM STATUS") | bold,
        separator(),
        // CPU and memory are percentages, so their sparklines use a fixed 0-100 scale.
        meter("CPU", t.cpuPercent / 100.0, pct(t.cpuPercent),
              sparkline(t.cpuHistory, kSparkWidth, 0.0, 100.0)),
        separatorLight(),
        meter("MEM", memFrac,
              std::to_string(static_cast<long>(memUsedKb / 1024.0)) + " / " +
              std::to_string(t.mem.totalKb / 1024) + " MB",
              sparkline(t.memHistory, kSparkWidth, 0.0, 100.0)),
        separatorLight(),
        diskRow,
        separator(),
        vbox(std::move(netRows)),
    });
}

// One mounted filesystem:  [bar]  used / total GB  /mount/point
// The path comes LAST, so a long path is never cut in the middle.
Element mountRow(const MountInfo& m) {
    const double total = static_cast<double>(m.disk.totalBytes);
    const double avail = static_cast<double>(m.disk.availableBytes);
    const double used = total > avail ? total - avail : 0.0;
    const double fraction = total > 0.0 ? used / total : 0.0;

    std::string value = gbText(used) + " / " + gbText(total) + " GB";
    if (m.readOnly) {
        value += "  RO";
    }

    Element row = hbox({
        text("") | size(WIDTH, EQUAL, 6),   // an empty label column keeps the bars aligned
        // A full read-only mount is normal (kernel headers, squashfs, ...), so no amber for it.
        gaugeBar(fraction, !m.readOnly) | size(WIDTH, EQUAL, 20),
        text(" " + value + "  " + m.mountPoint),
    });

    return m.readOnly ? row | dim : row;   // read-only mounts are shown, but quietly
}

// The SYSTEM DETAILS screen: uptime, load, swap, per-core CPU, mounts.
Element detailsPanel(const Telemetry& t, bool diskOn) {
    const std::size_t cores = t.corePercent.size();

    // Load average means "how many processes wanted a CPU, on average". Above the number
    // of cores, work is waiting in line, so the line turns amber.
    Element loadLine = text("LOAD AVG   " + fixed2(t.load.one) + "  " + fixed2(t.load.five) +
                            "  " + fixed2(t.load.fifteen) + "   (1 / 5 / 15 MIN)");
    if (cores > 0 && t.load.one > static_cast<double>(cores)) {
        loadLine = loadLine | color(kAmber);
    }

    // Swap is disk space used as overflow memory. A busy swap means RAM ran short.
    Element swapRow;
    if (t.mem.swapTotalKb > 0) {
        double swapUsedKb = static_cast<double>(t.mem.swapTotalKb - t.mem.swapFreeKb);
        swapRow = meter("SWAP", swapUsedKb / static_cast<double>(t.mem.swapTotalKb),
                        std::to_string(static_cast<long>(swapUsedKb / 1024.0)) + " / " +
                        std::to_string(t.mem.swapTotalKb / 1024) + " MB",
                        "", 6, 20);
    } else {
        swapRow = text("SWAP    NONE CONFIGURED") | dim;
    }

    // Per-core bars. With more than 8 cores they go into two columns to save height.
    Element coreBlock;
    if (cores == 0) {
        coreBlock = text("NO PER-CORE DATA") | dim;
    } else {
        const std::size_t shownCores = std::min(cores, kMaxCoresShown);
        const std::size_t columns = shownCores > 8 ? 2 : 1;
        const std::size_t perColumn = (shownCores + columns - 1) / columns;   // divides, rounding up

        Elements columnBoxes;
        for (std::size_t col = 0; col < columns; ++col) {
            Elements rows;
            for (std::size_t i = col * perColumn; i < shownCores && i < (col + 1) * perColumn; ++i) {
                rows.push_back(meter("C" + std::to_string(i), t.corePercent[i] / 100.0,
                                     pct(t.corePercent[i]), "", 6, 20));
            }
            columnBoxes.push_back(vbox(std::move(rows)) | flex);
            if (col + 1 < columns) {
                columnBoxes.push_back(text("  "));   // a gap between the two columns
            }
        }

        Elements coreRows;
        coreRows.push_back(hbox(std::move(columnBoxes)));
        if (cores > shownCores) {
            coreRows.push_back(text("... +" + std::to_string(cores - shownCores) + " MORE CORES") | dim);
        }
        coreBlock = vbox(std::move(coreRows));
    }

    // Mounted filesystems need the DISK MONITOR chip.
    Elements mountRows;
    if (!diskOn) {
        mountRows.push_back(text("[" + kChipDisk + " NOT INSTALLED]") | dim);
    } else if (t.mounts.empty()) {
        mountRows.push_back(text("READING...") | dim);   // the first reading arrives within a second
    } else {
        std::size_t shown = 0;
        for (const MountInfo& m : t.mounts) {
            if (shown >= kMaxMountsShown) break;
            mountRows.push_back(mountRow(m));
            ++shown;
        }
        if (t.mounts.size() > shown) {
            mountRows.push_back(text("... +" + std::to_string(t.mounts.size() - shown) + " MORE") | dim);
        }
    }

    return vbox({
        text("SYSTEM DETAILS") | bold,
        separator(),
        text("UPTIME     " + formatDuration(t.uptimeSeconds) + "  (H:M:S)"),
        loadLine,
        swapRow,
        separator(),
        text("CPU CORES (" + std::to_string(cores) + ")") | dim,
        coreBlock,
        separator(),
        text("MOUNTED FILESYSTEMS") | dim,
        vbox(std::move(mountRows)),
    });
}

// Returns the last "count" lines of a text file (used by the SYSTEM LOGS screen).
std::vector<std::string> readLogTail(const std::string& path, std::size_t count) {
    std::ifstream file(path);   // a missing file just gives an empty list
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    if (lines.size() > count) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(count));
    }
    return lines;
}

// ============================================================================
// main
// ============================================================================

// argc = number of command-line arguments, argv = the arguments as C strings.
// argv[0] is the program name itself, so real arguments start at index 1.
int main(int argc, char** argv) {
    // ------------------------------------------------------------------
    // COMMAND-LINE FLAGS that print something and exit (no UI, no prompt)
    // ------------------------------------------------------------------
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        // --json: print ONE snapshot as JSON and exit.
        if (arg == "--json") {
            ChipManager chips(4);
            if (!loadLoadout(chips, "loadout.json")) {
                applyDefaultLoadout(chips);
            }
            std::cout << takeSnapshotJson(chips) << "\n";
            return 0;
        }

        // --forget-programs: delete the saved list of known programs and exit.
        if (arg == "--forget-programs") {
            std::error_code ec;   // remove() reports problems through "ec" instead of throwing
            const bool removed = std::filesystem::remove(kKnownProgramsFile, ec);
            std::cout << (removed ? "Known-program list cleared.\n"
                                  : "No known-program list to clear.\n");
            return 0;
        }

        // --init-config: write a config file with every setting at its default.
        // It never overwrites an existing file.
        if (arg == "--init-config") {
            std::error_code ec;
            if (std::filesystem::exists(kConfigFile, ec)) {
                std::cout << kConfigFile << " already exists: not overwritten.\n";
                return 1;
            }
            std::ofstream out(kConfigFile);
            if (!out) {
                std::cout << "Could not write " << kConfigFile << ".\n";
                return 1;
            }
            out << defaultConfigJson();
            std::cout << "Wrote " << kConfigFile << ". Edit it, then check it with --check-config.\n";
            return 0;
        }

        // --check-config: show how the config file was understood, then exit.
        // The exit code is 0 when there are no problems, so scripts can use it too.
        if (arg == "--check-config") {
            std::vector<std::string> problems;
            const AppConfig cfg = loadConfig(kConfigFile, problems);
            std::error_code ec;
            std::cout << "Config file: " << kConfigFile
                      << (std::filesystem::exists(kConfigFile, ec) ? "" : "  (not found: using defaults)")
                      << "\n"
                      << "  gauge_warn_percent     " << numText(cfg.gaugeWarnPercent) << "\n"
                      << "  cpu_alert_percent      " << numText(cfg.cpuAlertPercent) << "\n"
                      << "  mem_alert_percent      " << numText(cfg.memAlertPercent) << "\n"
                      << "  process_alert_percent  " << numText(cfg.processAlertPercent) << "\n"
                      << "  heartbeat_seconds      "
                      << (cfg.heartbeatSeconds == 0 ? "off" : std::to_string(cfg.heartbeatSeconds)) << "\n"
                      << "  expected_ports         ";
            if (cfg.expectedPorts.empty()) {
                std::cout << "(none)";
            }
            for (unsigned int port : cfg.expectedPorts) {
                std::cout << port << " ";
            }
            std::cout << "\n";

            std::cout << "  comms_port             " << cfg.commsPort << "\n"
                      << "  comms_bind             " << cfg.commsBind << "\n"
                      << "  comms_connect          ";
            if (cfg.commsConnect.empty()) {
                std::cout << "(none)";
            }
            for (const HostPort& hp : cfg.commsConnect) {
                std::cout << hp.host << ":" << hp.port << " ";
            }
            std::cout << "\n  comms_allow            ";
            if (cfg.commsAllow.empty()) {
                std::cout << "(none)";
            }
            for (const std::string& host : cfg.commsAllow) {
                std::cout << host << " ";
            }
            std::cout << "\n";

            if (problems.empty()) {
                std::cout << "No problems found.\n";
            } else {
                std::cout << "PROBLEMS (these settings were ignored):\n";
                for (const std::string& problem : problems) {
                    std::cout << "  - " << problem << "\n";
                }
            }
            return problems.empty() ? 0 : 1;
        }
    }

    logEvent(LogLevel::Info, "POD-042 Booting");

    // ------------------------------------------------------------------
    // SETTINGS: config.json (a missing file = all defaults)
    // ------------------------------------------------------------------
    std::vector<std::string> configProblems;
    const AppConfig config = loadConfig(kConfigFile, configProblems);
    for (const std::string& problem : configProblems) {
        logEvent(LogLevel::Warning, "Config: " + problem);   // visible on the SYSTEM LOGS screen
    }
    gWarnFraction = config.gaugeWarnPercent / 100.0;   // set once, before any thread starts

    // ------------------------------------------------------------------
    // OPERATOR NAME: loaded from profile.json, or asked once on the first run.
    // Must happen BEFORE the fullscreen UI, because std::cin doesn't work
    // once FTXUI takes over the terminal.
    // ------------------------------------------------------------------
    bool askForName = false;   // "./build/pod042 --rename" forces the prompt again
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--rename") {   // wrap in std::string so == compares text
            askForName = true;
        }
    }

    std::string userName;
    if (askForName || !loadName(userName, "profile.json")) {
        std::cout << "POD 042: ENTER OPERATOR DESIGNATION: ";
        std::getline(std::cin, userName);
        userName = trimSpaces(userName);
        if (userName.empty()) {
            userName = "UNIT";
        }
        saveName(userName, "profile.json");
    }

    // ------------------------------------------------------------------
    // 1. CHIPS
    // ------------------------------------------------------------------
    ChipManager chips(4);
    if (!loadLoadout(chips, "loadout.json")) {
        applyDefaultLoadout(chips);   // first run: no saved loadout yet
    }

    std::vector<std::string> chipLabels;   // the text of each menu line
    int chipSelected = 0;                  // index of the highlighted chip
    std::string chipMessage;               // feedback line, e.g. "NOT ENOUGH SLOTS"

    // "Which chips are on" as thread-safe flags. The UI thread WRITES them (inside
    // updateLabels); the other threads only READ them. The ChipManager itself is never
    // touched from another thread: that would be a data race.
    std::atomic<bool> netEnabled{false};
    std::atomic<bool> diskEnabled{false};
    std::atomic<bool> logEnabled{false};
    std::atomic<bool> processEnabled{false};
    std::atomic<bool> commsEnabled{false};

    // A LAMBDA: a small unnamed function stored in a variable. [&] lets it use the
    // variables around it (chips, chipLabels, ...) by reference. This one rebuilds
    // the menu text AND refreshes the flags, so every install/remove updates both.
    auto updateLabels = [&] {
        chipLabels.clear();
        for (const Chip& c : chips.chips()) {
            chipLabels.push_back((c.enabled ? "[ON]  " : "[OFF] ") + c.name +
                                 "  (cost " + std::to_string(c.cost) + ")");
        }
        netEnabled     = chips.isEnabled(kChipNetwork);
        diskEnabled    = chips.isEnabled(kChipDisk);
        logEnabled     = chips.isEnabled(kChipLog);
        processEnabled = chips.isEnabled(kChipProcess);
        commsEnabled   = chips.isEnabled(kChipComms);
    };
    updateLabels();

    // Enter on a chip toggles it, then saves the loadout.
    MenuOption chipOption = MenuOption::Vertical();
    chipOption.on_enter = [&] {
        std::string name = chips.chips()[static_cast<std::size_t>(chipSelected)].name;
        bool wasEnabled = chips.isEnabled(name);

        ChipResult result = wasEnabled ? chips.disable(name) : chips.enable(name);
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
        updateLabels();
        saveLoadout(chips, "loadout.json");
    };

    // ------------------------------------------------------------------
    // 2. TELEMETRY: shared data
    // ------------------------------------------------------------------
    // Two threads (UI and refresher) use "telemetry". A mutex is a lock: only the
    // thread holding it may touch the data; the other waits.
    std::mutex telemetryMutex;
    Telemetry telemetry;
    telemetry.mem = readMemInfo();   // fill once now so the first frame isn't empty
    if (diskEnabled) {
        telemetry.disk = readDiskInfo("/");
    }

    // Returns a COPY of the shared telemetry; the lock is held only while copying.
    auto snapshotTelemetry = [&] {
        std::lock_guard<std::mutex> lock(telemetryMutex);
        return telemetry;
    };

    auto screen = ScreenInteractive::Fullscreen();

    // atomic<bool>: a bool that is safe to read in one thread and write in another.
    std::atomic<bool> running{true};

    // ---- COMMS: messages between pods ----
    // "comms" is declared AFTER "screen", so it is destroyed BEFORE it: the node's thread
    // calls screen.PostEvent, so the screen must outlive the node.
    CommsConfig commsConfig;
    commsConfig.localName = trimSpaces(sanitizeText(userName, kMaxPeerNameBytes));
    if (commsConfig.localName.empty()) {
        commsConfig.localName = "UNIT";
    }
    commsConfig.bindAddress = config.commsBind;
    commsConfig.port = config.commsPort;
    commsConfig.connectTo = config.commsConnect;
    commsConfig.allowFrom = config.commsAllow;

    CommsNode comms;
    // Whenever a message arrives or a link changes, ask the UI to redraw right away.
    comms.setOnChange([&] { screen.PostEvent(Event::Custom); });

    // ------------------------------------------------------------------
    // 3. THE REFRESHER THREAD: reads the system once per second
    // ------------------------------------------------------------------
    std::thread refresher([&] {
        // Rates (CPU, network) need a PREVIOUS reading, so we keep one.
        cpuInfo prevCpu = readCpuInfo();
        std::vector<cpuInfo> prevCores = readCoreCpuInfo();
        NetInfo prevNet;                     // all zeros until the first real reading
        auto prevTime = std::chrono::steady_clock::now();

        // false while the network chip is off. After the chip is (re)installed, the
        // first reading only primes prevNet; real rates start with the second.
        bool netPrimed = false;

        bool cpuAlertActive = false;         // log only when the state CHANGES

        // For the Pod heartbeat: when the thread started, and when we last reported.
        const auto startTime = std::chrono::steady_clock::now();
        auto lastHeartbeat = startTime;

        // PROCESS WATCH state.
        ProcessSampler processSampler;       // keeps its own previous sample
        std::set<int> alertedPids;           // processes currently flagged as high CPU

        // The event feed: compares each sample with the previous one.
        ChangeDetector detector;
        // Start from the programs seen in earlier runs (empty on the very first run).
        detector.loadKnownPrograms(loadKnownPrograms(kKnownProgramsFile));
        std::size_t savedProgramCount = detector.knownPrograms().size();

        // Writes detected changes to the log. Every check below passes its result through here.
        auto publish = [&](std::vector<Change> changes) {
            if (changes.empty()) {
                return;
            }
            limitChanges(changes, kMaxChangesPerSample);

            // Finding a socket's owner scans every process's open files, which is expensive.
            // Do it only when a change involves a socket AND PROCESS WATCH is installed.
            std::map<unsigned long long, SocketOwner> owners;
            const bool needOwners = processEnabled &&
                std::any_of(changes.begin(), changes.end(),
                            [](const Change& c) { return c.inode != 0; });
            if (needOwners) {
                owners = readSocketOwners();
            }

            for (const Change& c : changes) {
                std::string message = c.message;
                if (c.inode != 0) {
                    auto found = owners.find(c.inode);
                    if (found != owners.end()) {
                        message += " [" + found->second.name + " (pid " +
                                   std::to_string(found->second.pid) + ")]";
                    }
                }
                logEvent(c.level, message);
            }
        };

        // History for the sparklines: one value per second, oldest first.
        History cpuHist(kHistorySamples);
        History memHist(kHistorySamples);
        History rxHist(kHistorySamples);
        History txHist(kHistorySamples);

        // Mounted filesystems change rarely and cost one statvfs() call each, so they are
        // read every 5th second (and immediately when the chip is installed).
        std::vector<MountInfo> mounts;
        bool mountsWereOn = false;
        int mountTick = 0;

        while (running) {
            // Sleep 1 second as ten 100 ms naps, so quitting is noticed within 100 ms.
            for (int i = 0; i < 10 && running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!running) break;

            // COMMS chip: keep the message link in step with the chip.
            if (commsEnabled && !comms.running()) {
                comms.start(commsConfig);
            } else if (!commsEnabled && comms.running()) {
                comms.stop();
            }

            // Second sample for the rates.
            cpuInfo nowCpu = readCpuInfo();
            std::vector<cpuInfo> nowCores = readCoreCpuInfo();
            auto nowTime = std::chrono::steady_clock::now();
            double seconds = std::chrono::duration<double>(nowTime - prevTime).count();

            Telemetry t;                     // a fresh snapshot; fields default to zero
            t.mem = readMemInfo();           // core systems: always read
            t.cpuPercent = cpuUsagePercent(prevCpu, nowCpu);
            t.load = readLoadAvg();
            t.uptimeSeconds = readUptimeSeconds();
            t.corePercent = coreUsage(prevCores, nowCores);

            const double memPercent = t.mem.totalKb > 0
                ? 100.0 * static_cast<double>(t.mem.totalKb - t.mem.availableKb) /
                      static_cast<double>(t.mem.totalKb)
                : 0.0;
            cpuHist.push(t.cpuPercent);
            memHist.push(memPercent);

            // DISK MONITOR chip: disk usage and the mount list.
            if (diskEnabled) {
                t.disk = readDiskInfo("/");
                if (!mountsWereOn || mountTick % 5 == 0) {
                    mounts = readMounts();
                }
                ++mountTick;
                mountsWereOn = true;
                t.mounts = mounts;
            } else {
                mounts.clear();
                mountsWereOn = false;
                mountTick = 0;   // reinstalling the chip reads right away
            }

            // PROCESS WATCH chip: sample all processes, only while the chip is installed.
            if (processEnabled) {
                std::vector<ProcessInfo> procs = processSampler.sample(nowCpu);
                t.processCount = procs.size();
                publish(detector.checkProcesses(procs));   // log programs seen for the first time

                // Save the list whenever it grew. We update the counter even if the save
                // failed: otherwise a failing disk would retry (and log) every second.
                if (detector.knownPrograms().size() != savedProgramCount) {
                    saveKnownPrograms(detector.knownPrograms(), kKnownProgramsFile);
                    savedProgramCount = detector.knownPrograms().size();
                }

                // Log a warning when a process CROSSES the threshold (not every second).
                std::set<int> stillHigh;
                for (const ProcessInfo& p : procs) {
                    if (p.cpuPercent > config.processAlertPercent) {
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
                processSampler.reset();      // chip removed: forget the old sample
                alertedPids.clear();
                detector.resetProcesses();   // reinstalling starts a fresh baseline
            }

            // NETWORK SCAN chip: speeds, sparklines and the connection feed.
            if (netEnabled) {
                NetInfo nowNet = readNetInfo();
                if (netPrimed) {
                    t.net = netRates(prevNet, nowNet, seconds);
                    rxHist.push(t.net.rxBytesPerSec);
                    txHist.push(t.net.txBytesPerSec);
                }
                prevNet = nowNet;
                netPrimed = true;

                // Event feed: new listeners and new connections since the last sample.
                publish(detector.checkConnections(readConnections()));
            } else {
                netPrimed = false;               // forget the old reading
                detector.resetConnections();     // and the old connection list
                rxHist.clear();                  // an old graph would be misleading
                txHist.clear();
            }

            // System CPU alert: log only when the state changes (normal <-> high).
            bool cpuHigh = t.cpuPercent > config.cpuAlertPercent;
            if (cpuHigh && !cpuAlertActive) {
                logEvent(LogLevel::Warning, "CPU usage high: " + pct(t.cpuPercent));
            } else if (!cpuHigh && cpuAlertActive) {
                logEvent(LogLevel::Info, "CPU usage back to normal");
            }
            cpuAlertActive = cpuHigh;

            // Pod heartbeat: once every heartbeat_seconds (0 in config.json = off).
            if (config.heartbeatSeconds > 0 &&
                nowTime - lastHeartbeat >= std::chrono::seconds(config.heartbeatSeconds)) {
                double uptime = std::chrono::duration<double>(nowTime - startTime).count();
                logEvent(LogLevel::Info, podHeartbeat(t.cpuPercent, memPercent, uptime));
                lastHeartbeat = nowTime;
            }

            // Copy the histories into the snapshot the UI will read.
            t.cpuHistory = cpuHist.values();
            t.memHistory = memHist.values();
            t.rxHistory = rxHist.values();
            t.txHistory = txHist.values();

            {
                // lock_guard locks the mutex here and unlocks it at the closing brace.
                std::lock_guard<std::mutex> lock(telemetryMutex);
                telemetry = t;   // swap in the new snapshot
            }

            // This sample becomes the "previous" one for the next round.
            prevCpu = nowCpu;
            prevCores = nowCores;
            prevTime = nowTime;

            // Ask the UI to redraw. PostEvent is safe to call from another thread.
            screen.PostEvent(Event::Custom);
        }
    });

    // ------------------------------------------------------------------
    // 4. BOOT SEQUENCE (typewriter)
    // ------------------------------------------------------------------
    // Some lines contain real data (the chip loadout and the operator name).
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

    // How many bytes of the boot text are visible so far, counted across ALL lines
    // as if they were one long text.
    std::atomic<std::size_t> bootChars{0};

    // Total size of the boot text, used for the progress bar.
    std::size_t totalBootChars = 0;
    for (const std::string& line : bootLines) {
        totalBootChars += line.size();
    }

    // Sleeps "ms" milliseconds in 10 ms slices; wakes early on skip or quit.
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
                break;
            }
            if (remaining >= line.size()) {
                rows.push_back(text(line));
                remaining -= line.size();
            } else {
                rows.push_back(text(line.substr(0, remaining) + "█"));   // block cursor
                remaining = 0;
            }
        }
        if (shown == 0) {
            rows.push_back(text("█"));
        }

        rows.push_back(separator());
        rows.push_back(gauge(static_cast<float>(shown) / static_cast<float>(totalBootChars)));
        rows.push_back(text("PRESS ANY KEY TO SKIP") | dim);

        return vbox(std::move(rows)) | border | size(WIDTH, EQUAL, 56) | center;
    };

    // ------------------------------------------------------------------
    // 5. UI COMPONENTS
    //
    // Element   = something you DRAW (text, gauge, vbox).
    // Component = something that draws AND reacts to input (a menu, a tab view).
    // Renderer(...) turns a function that returns an Element into a Component.
    // ------------------------------------------------------------------

    // Left menu: the list of screens. "tab" holds the selected index; the menu
    // stores the address of "tab" and updates it as you move.
    std::vector<std::string> tabNames = {"SYSTEM STATUS", "SYSTEM DETAILS",
                                         "NETWORK CONNECTIONS", "PROCESS WATCH",
                                         "CHIP CONFIGURATION", "COMMS", "SYSTEM LOGS"};
    int tab = 0;
    Component tabMenu = Menu(&tabNames, &tab, MenuOption::Vertical());

    // Screen 0: status.
    Component statusView = Renderer([&] {
        return statusPanel(snapshotTelemetry(), diskEnabled, netEnabled);
    });

    // Screen 1: details (uptime, load, swap, per-core CPU, mounts).
    Component detailsView = Renderer([&] {
        return detailsPanel(snapshotTelemetry(), diskEnabled);
    });

    // Screen 2: network connections. Only works if the NETWORK SCAN chip is installed.
    // It reads /proc/net/tcp(6) directly each time it's drawn: cheap, and it keeps this
    // screen free of shared state, so no mutex is needed.
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

            // A LISTEN socket bound to 0.0.0.0 or :: accepts connections on EVERY interface,
            // so it is reachable from outside. Amber, unless its port is in expected_ports.
            if (c.state == "LISTEN" && isWildcardAddress(c.localAddr) &&
                config.expectedPorts.count(c.localPort) == 0) {
                row = row | color(kAmber);
            }
            rows.push_back(row);
            ++shown;
        }

        if (conns.size() > shown) {
            rows.push_back(text("... +" + std::to_string(conns.size() - shown) + " more") | dim);
        }
        rows.push_back(separator());
        rows.push_back(text(std::string(config.expectedPorts.empty()
                                ? "AMBER = LISTENING ON ALL INTERFACES (0.0.0.0 / ::)."
                                : "AMBER = ALL INTERFACES, PORT NOT IN expected_ports.") +
                            "  ? = OWNER NOT VISIBLE WITHOUT ROOT") | dim);
        return vbox(std::move(rows));
    });

    // Screen 3: process watch. Only works if the PROCESS WATCH chip is installed.
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
            if (p.cpuPercent > config.processAlertPercent) {
                row = row | color(kAmber);   // same threshold as the log warning
            }
            rows.push_back(row);
        }

        rows.push_back(separator());
        rows.push_back(text("AMBER = CPU ABOVE " + numText(config.processAlertPercent) +
                            "% (ALSO LOGGED). 100% = ONE FULL CORE.") | dim);
        return vbox(std::move(rows));
    });

    // Screen 4: chips (interactive menu plus a slot gauge and the presets).
    Component chipMenu = Menu(&chipLabels, &chipSelected, chipOption);
    Component chipsView = Renderer(chipMenu, [&] {
        double used = chips.usedSlots();
        double total = chips.totalSlots();
        const Chip& selected = chips.chips()[static_cast<std::size_t>(chipSelected)];

        // Build "PRESETS:  [1] DIAGNOSTIC  [2] EXPLORATION ..." from the preset list,
        // so adding a preset in loadout.cpp updates the screen too.
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

    // Screen 5: comms (messages to other pods).
    std::string commsText;   // what the user is typing; the Input component edits it directly
    InputOption commsOption;
    commsOption.on_enter = [&] {   // called when ENTER is pressed inside the box
        if (!commsEnabled) {
            return;
        }
        if (comms.send(commsText)) {
            commsText.clear();     // sent: empty the box
        }
    };
    Component commsInput = Input(&commsText, "type a message, ENTER to send", commsOption);

    // Renderer(child, fn): the child gets the key presses, fn draws everything.
    Component commsView = Renderer(commsInput, [&]() -> Element {
        if (!commsEnabled) {
            return vbox({
                text("COMMS") | bold,
                separator(),
                text(kChipComms + " CHIP NOT INSTALLED") | color(kAmber),
                text("Install it on the CHIP CONFIGURATION screen.") | dim,
            });
        }

        const CommsSnapshot snap = comms.snapshot();   // one consistent copy for this frame

        std::string mode = "STARTING...";
        if (snap.running) {
            mode = snap.listening ? "LISTENING ON " + snap.listenText : "NOT LISTENING (DIAL-OUT ONLY)";
        }

        Elements linkRows;
        if (snap.links.empty()) {
            linkRows.push_back(text("  NO LINKS") | dim);
        }
        for (const CommsLink& link : snap.links) {
            const std::string who = link.ready ? link.name : "(CONNECTING)";
            linkRows.push_back(text(std::string(link.ready ? "  LINKED   " : "  PENDING  ") + who +
                                    "  " + link.address + (link.outbound ? "  (OUT)" : "  (IN)")));
        }

        // Newest first, so the box and the latest messages stay at the top of the screen.
        Elements history;
        const std::size_t total = snap.lines.size();
        const std::size_t shownCount = std::min<std::size_t>(total, 10);
        for (std::size_t i = 0; i < shownCount; ++i) {
            const CommsLine& line = snap.lines[total - 1 - i];
            // paragraph() wraps long text over several lines instead of cutting it off.
            Element row = paragraph("[" + line.time + "] " + line.text);
            if (line.kind == CommsKind::Outgoing) {
                row = row | dim;
            } else if (line.kind == CommsKind::System) {
                row = row | color(kAmber);
            }
            history.push_back(row);
        }
        if (history.empty()) {
            history.push_back(text("NO TRANSMISSIONS YET") | dim);
        }

        return vbox({
            text("COMMS") | bold,
            separator(),
            text(mode),
            text("UNENCRYPTED LINK: TRUSTED LOCAL NETWORKS ONLY") | color(kAmber),
            separator(),
            vbox(std::move(linkRows)),
            separator(),
            hbox({text("> "), commsInput->Render() | flex}) | border,
            vbox(std::move(history)),
            separator(),
            text("RIGHT: focus message box   ENTER: send   ESC: back to menu") | dim,
        });
    });

    // Screen 6: the last lines of the event log. Only works if LOG STREAM is installed
    // (the log FILE is written either way; the chip only controls viewing).
    Component logsView = Renderer([&]() -> Element {
        if (!logEnabled) {
            return vbox({
                text("SYSTEM LOGS") | bold,
                separator(),
                text(kChipLog + " CHIP NOT INSTALLED") | color(kAmber),
                text("Install it on the CHIP CONFIGURATION screen.") | dim,
            });
        }

        Elements rows;
        rows.push_back(text("SYSTEM LOGS") | bold);
        rows.push_back(separator());
        for (const std::string& line : readLogTail("pod042.log", 20)) {
            // find() returns std::string::npos when the text is NOT found.
            Element row = text(line);
            if (line.find("[WARN]") != std::string::npos) {
                row = row | color(kAmber);
            } else if (line.find("[ERROR]") != std::string::npos) {
                row = row | color(Color::Red);
            }
            rows.push_back(row);
        }
        return vbox(std::move(rows));   // std::move hands the vector over without copying
    });

    // Container::Tab shows only the panel selected by the left menu (same order as tabNames!).
    Component tabs = Container::Tab(
        {statusView, detailsView, connectionsView, processesView, chipsView, commsView, logsView}, &tab);

    // Container::Horizontal groups the left menu and the right panel, so
    // Left/Right arrow keys move focus between them.
    Component layout = Container::Horizontal({tabMenu, tabs});

    // The root component: boot screen first, then header + menu + active screen + footer.
    Component root = Renderer(layout, [&]() -> Element {
        if (booting) {
            return bootScreen();
        }

        // The header's status line reflects the live CPU state.
        Telemetry snap = snapshotTelemetry();
        Element statusLine = snap.cpuPercent > config.cpuAlertPercent
            ? text("POD 042: WARNING. CPU LOAD EXCEEDS THRESHOLD.") | bold | color(kAmber)
            : text("SYSTEM STATUS: OPERATIONAL");

        return vbox({
            vbox({
                text("YoRHa // UNIT DIAGNOSTICS") | bold,
                text("NAME: " + userName),
                statusLine,
            }) | border,
            hbox({
                tabMenu->Render() | border | size(WIDTH, EQUAL, 28),
                tabs->Render() | border | flex,
            }) | flex,   // flex here = take all remaining vertical space
            text(" UP/DOWN: select   LEFT/RIGHT: switch focus   Q: quit ") | dim,
        });
    });

    // CatchEvent wraps a component and sees every key press first. Returning true
    // means "handled" (nothing else sees the key); false lets the components process it.
    root = CatchEvent(root, [&](Event event) {
        // During the boot sequence, any key skips it.
        if (booting) {
            // Our own redraw requests and mouse movement must not skip the boot.
            if (event == Event::Custom || event.is_mouse()) {
                return false;
            }
            booting = false;
            return true;
        }

        // While the message box has focus, EVERY key belongs to it: typing "quit" must not
        // close the program, and typing "3" must not switch presets.
        // ESC is the way out: it hands the focus back to the left-hand menu.
        if (tab == kTabComms && commsEnabled && commsInput->Focused()) {
            if (event == Event::Escape) {
                tabMenu->TakeFocus();
                return true;
            }
            return false;   // not handled here: the Input component gets the key
        }

        // On the chip screen, keys 1..9 load the matching preset.
        // event.is_character() is true for printable keys; event.character() is that key
        // as a std::string, so [0] is the key itself.
        if (tab == kTabChips && event.is_character()) {
            char key = event.character()[0];
            if (key >= '1' && key <= '9') {
                selectPreset(static_cast<std::size_t>(key - '1'));   // '1' -> 0, '2' -> 1, ...
                return true;
            }
        }

        if (event == Event::Character('q') || event == Event::Character('Q')) {
            // ExitLoopClosure() returns a function that stops the app; the second () calls it.
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    // ------------------------------------------------------------------
    // 6. RUN: blocks here, drawing and handling keys, until Q is pressed
    // ------------------------------------------------------------------
    screen.Loop(root);

    // ------------------------------------------------------------------
    // 7. SHUTDOWN: stop every background thread cleanly
    // ------------------------------------------------------------------
    running = false;     // tell the refresher loop to stop
    booting = false;     // and the boot thread
    refresher.join();    // wait until each thread has actually finished.
    bootThread.join();   // (Destroying a std::thread that was never joined crashes the program.)
    comms.stop();        // closes every socket and joins the I/O thread
    logEvent(LogLevel::Info, "POD-042 Shutdown");
    return 0;
}