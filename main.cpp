// ---- Standard library headers ----
#include <atomic>    // std::atomic: a variable that is safe to share between threads
#include <chrono>    // time utilities (durations, clocks)
#include <cstdio>    // std::snprintf: formats numbers into text
#include <fstream>   // std::ifstream: reads files (used to show the log)
#include <mutex>     // std::mutex: a lock that stops two threads touching data at once
#include <string>
#include <thread>    // std::thread: runs a function in the background
#include <vector>

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

using namespace ftxui;

struct Telemetry {
    MemInfo mem;
    DiskInfo disk;
    double cpuPercent = 0.0;
    NetRates net;
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
    return hbox({
        text(label) | size(WIDTH, EQUAL, 6), // width exactly 6 characters
        gauge(static_cast<float>(fraction)) | flex, // flex to fill leftover space
        text(" " + value),
    });
}

Element statusPanel(const Telemetry& t){
    // constexpr = constant computed at compile time.
    constexpr double GB = 1024.0 * 1024.0 * 1024.0;

    double memUsedKb = static_cast<double>(t.mem.totalKb - t.mem.availableKb);
    double memFrac = t.mem.totalKb > 0 ? memUsedKb / static_cast<double>(t.mem.totalKb) : 0.0;

    double diskUsed = static_cast<double>(t.disk.totalBytes - t.disk.availableBytes);
    double diskFrac = t.disk.totalBytes > 0 ? diskUsed / static_cast<double>(t.disk.totalBytes) : 0.0;

    return vbox({
        text("SYSTEM STATUS") | bold,
        separator(),
        meter("CPU", t.cpuPercent / 100.0, pct(t.cpuPercent)),
        
        separatorEmpty(),

        meter("MEM", memFrac, 
            std::to_string(static_cast<long>(memUsedKb / 1024.0)) + " / " + 
            std::to_string(t.mem.totalKb / 1024) + " MB"),
        
        separatorEmpty(),

        meter("DISK", diskFrac,
            std::to_string(static_cast<long>(diskUsed / GB)) + " / " +
            std::to_string(static_cast<long>(static_cast<double>(t.disk.totalBytes) / GB)) + " GB"),
        separator(),
        text("NETWORK"),
        text("  DOWN  " + kbps(t.net.rxBytesPerSec)),
        text("  UP  " + kbps(t.net.txBytesPerSec)),
        separator(),
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

int main() {

    logEvent(LogLevel::Info, "POD-042 Booting");

    // =====================================================
    // 1. CHIPS: create manager and restore saved loadout.
    // =====================================================

    ChipManager chips(4);

    // Try to restore the saved loadout; on the first run there's no file yet.
    if (!loadLoadout(chips, "loadout.json")) {
        chips.enable("NETWORK SCAN");   // default loadout: 2 of 4 slots
        chips.enable("LOG STREAM");
}

    std::vector<std::string> chipLabels; // text of each menu line
    int chipSelected = 0; // index of highlighted chip
    std::string chipMessage; // feedback line eg not enough slots

    // lambda: small unnamed function stored  in a variable
    // [&] = capture: the lambda can use the variables around 
    // it by reference, not by copy
    // this one rebuilds menu text from the chip manager's current state
    // so ON and OFF is always up to date

    auto updateLabels = [&] {
        chipLabels.clear();
        for(const Chip& c : chips.chips()) {
            chipLabels.push_back((c.enabled ? "[ON]" : "[OFF]") + c.name + 
                                " (cost " + std::to_string(c.cost) + ")"); 
        }
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

    // =====================================================
    // 2. TELEMETRY
    // =====================================================
    // Two threads, use mutex for locking.
    std::mutex telemtryMutex;
    Telemetry telemetry;
    telemetry.mem = readMemInfo();
    telemetry.disk = readDiskInfo("/");

    auto screen = ScreenInteractive::Fullscreen();

    // bool safe to read in one thread and wrie in another without mutex
    // refresher checks it to know when to stop
    std::atomic<bool> running{true};

    std::thread refresher([&] {
        cpuInfo prevCpu = readCpuInfo();
        NetInfo prevNet = readNetInfo();

        auto prevTime = std::chrono::steady_clock::now();

        while(running) {
            // sleep 1 second
            for(int i = 0; i < 10 && running; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if(!running) break;

            cpuInfo nowCpu = readCpuInfo();
            NetInfo nowNet = readNetInfo();
            auto nowTime = std::chrono::steady_clock::now();

            double seconds = std::chrono::duration<double>(nowTime - prevTime).count();

            Telemetry t;
            t.mem  = readMemInfo();
            t.disk = readDiskInfo("/");
            t.cpuPercent = cpuUsagePercent(prevCpu, nowCpu);
            t.net = netRates(prevNet, nowNet, seconds);

            {
                std::lock_guard<std::mutex> lock(telemtryMutex);
                telemetry = t;
            }

            prevCpu = nowCpu;
            prevNet = nowNet;
            prevTime  = nowTime;

            screen.PostEvent(Event::Custom);
        }
    });

    // 3. UI
    // Element: something you draw
    // Component: something that draws and reacts to input
    // Renderer() turns a function that returns an element into a component

    // Left menu: list of screens. "tab" holds the selected index
    // menu stores the address of "tab" and updates it as you move
    std::vector<std::string> tabNames = {"SYSTEM STATUS", "CHIP CONFIGURATION", "SYSTEM LOGS"};
    int tab = 0;
    Component tabMenu = Menu(&tabNames, &tab, MenuOption::Vertical());

    // Screen 1: status
    // This lambda runs every time the screen is redrawn
    Component statusView = Renderer([&] {
        Telemetry snapshot;
        {
            // copy shared data while holding the lock, then release it
            std::lock_guard<std::mutex> lock(telemtryMutex);
            snapshot = telemetry;
        }
        return statusPanel(snapshot);
    });

    // Screen 2: chips
    // Remderer(child, fn): child receives key presses; fn decides how everything is drawm
    // Inside fn, chipMenu->Render() draws the list itself.
    Component chipMenu = Menu(&chipLabels, &chipSelected, chipOption);
    Component chipsView = Renderer(chipMenu, [&] {
        double used = chips.usedSlots();
        double total = chips.totalSlots();

        const Chip& selected = chips.chips()[static_cast<std::size_t>(chipSelected)];
        return vbox({
            text("CHIP CONFIGURATION") | bold,
            separator(),
            meter("SLOTS", total > 0 ? used / total : 0.0,
                std::to_string(chips.usedSlots()) + "/" + std::to_string(chips.totalSlots())),
            separator(),
            chipMenu->Render(), // list of chips
            separator(),
            text(selected.description) | dim, // dim = fainter text style
            text(chipMessage) | bold,
            text("RIGHT: focus list ENTER: install / remove") | dim,
        });
    });

    // Screen 3: the last 20 lines of the event log, re-read on every redraw
    Component logsView = Renderer([&] {
        Elements rows;
        rows.push_back(text("System LOGS") | bold);
        rows.push_back(separator());
        for(const std::string& line : readLogTail("pod042.log", 20)) {
            rows.push_back(text(line));
        }
        return vbox(std::move(rows));
    });
    // Container::Tab holds several components but shows only the one at index
    // "tab" (the left menu's selection).
    Component tabs = Container::Tab({statusView, chipsView, logsView}, &tab);

    // Container::Horizontal groups the left menu and the right panel, so
    // Left/Right arrow keys move focus between them.
    Component layout = Container::Horizontal({tabMenu, tabs});

    Component root = Renderer(layout, [&] {
        return vbox({
            // Header box
            vbox({
                text("YoRHa // UNIT DIAGNOSTICS") | bold,
                text("ANDROID ID: 2B-042"),
                text("SYSTEM STATUS: OPERATIONAL"),
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
        if(event==Event::Character('q') || event==Event::Character('Q')) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    // 4. RUN: blocks here, drawing and handling keys, until Q is pressed
    screen.Loop(root);

    // 5. SHUTDOWN: stop background thread cleanly
    running = false; // tell refresher loop to stop
    refresher.join(); // wait until it has actually finished
    logEvent(LogLevel::Info, "POD-042 Shutdown.");
    return 0;
}