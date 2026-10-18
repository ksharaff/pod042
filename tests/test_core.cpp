// ============================================================================
// test_core.cpp - unit tests for the pure logic of POD-042.
//
// GoogleTest basics (it's the C++ equivalent of JUnit):
//   TEST(SuiteName, TestName) { ... }   defines one test.
//   EXPECT_*(...)  records a failure but keeps running the test.
//   ASSERT_*(...)  records a failure and STOPS the test; use it when the rest of
//                  the test makes no sense after that failure.
// ============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "chips.h"
#include "connections.h"
#include "cpu.h"
#include "loadout.h"
#include "net.h"
#include "profile.h"
#include "changes.h"
#include "process.h"
#include "knownprograms.h"

// Helpers private to this file.
namespace {

// Builds a cpuInfo with just the three fields our tests care about.
cpuInfo makeCpu(long user, long idle, long iowait = 0) {
    cpuInfo c;   // every other field stays 0 (default from cpu.h)
    c.user = user;
    c.idle = idle;
    c.iowait = iowait;
    return c;
}

// A path inside the system's temp folder (/tmp), so tests never touch real data.
std::string tempFile(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

// Writes text to a file; used to create (deliberately broken) input files.
void writeFile(const std::string& path, const std::string& contents) {
    std::ofstream file(path);
    file << contents;
}

}  // namespace

// ---------------------------------------------------------------------------
// CPU usage math
// ---------------------------------------------------------------------------

TEST(CpuUsage, TenPercentBusy) {
    // Between the two readings 1000 ticks passed, and 900 of them were idle.
    cpuInfo before = makeCpu(100, 900);
    cpuInfo after  = makeCpu(200, 1800);
    // EXPECT_NEAR compares doubles with a tolerance (never use == on doubles).
    EXPECT_NEAR(cpuUsagePercent(before, after), 10.0, 0.001);
}

TEST(CpuUsage, FullyBusy) {
    EXPECT_NEAR(cpuUsagePercent(makeCpu(0, 0), makeCpu(1000, 0)), 100.0, 0.001);
}

TEST(CpuUsage, CompletelyIdle) {
    EXPECT_NEAR(cpuUsagePercent(makeCpu(0, 0), makeCpu(0, 1000)), 0.0, 0.001);
}

TEST(CpuUsage, IowaitCountsAsIdle) {
    // 100 ticks passed, all of them iowait: the CPU did no work, so 0%.
    EXPECT_NEAR(cpuUsagePercent(makeCpu(0, 0, 0), makeCpu(0, 0, 100)), 0.0, 0.001);
}

TEST(CpuUsage, NoTimePassedDoesNotDivideByZero) {
    // This is the "-inf%" bug from earlier: identical readings must give exactly 0.
    cpuInfo same = makeCpu(500, 500);
    EXPECT_DOUBLE_EQ(cpuUsagePercent(same, same), 0.0);
}

// ---------------------------------------------------------------------------
// Network rates
// ---------------------------------------------------------------------------

TEST(NetRates, ComputesBytesPerSecond) {
    NetInfo before;
    before.rxBytes = 1000;
    before.txBytes = 500;
    NetInfo after;
    after.rxBytes = 3048;   // +2048 bytes over 2 seconds
    after.txBytes = 1524;   // +1024 bytes over 2 seconds

    NetRates r = netRates(before, after, 2.0);
    EXPECT_DOUBLE_EQ(r.rxBytesPerSec, 1024.0);
    EXPECT_DOUBLE_EQ(r.txBytesPerSec, 512.0);
}

TEST(NetRates, CounterResetGivesZeroNotHugeNumber) {
    // If a counter goes DOWN (interface reset), unsigned subtraction would wrap
    // around to a gigantic number. The code must report 0 instead.
    NetInfo before;
    before.rxBytes = 5000;
    before.txBytes = 5000;
    NetInfo after;
    after.rxBytes = 100;
    after.txBytes = 100;

    NetRates r = netRates(before, after, 1.0);
    EXPECT_DOUBLE_EQ(r.rxBytesPerSec, 0.0);
    EXPECT_DOUBLE_EQ(r.txBytesPerSec, 0.0);
}

TEST(NetRates, ZeroSecondsGivesZero) {
    NetInfo a;
    NetInfo b;
    b.rxBytes = 999;
    EXPECT_DOUBLE_EQ(netRates(a, b, 0.0).rxBytesPerSec, 0.0);
}

// ---------------------------------------------------------------------------
// /proc/net/tcp decoding
// ---------------------------------------------------------------------------

TEST(Connections, HexToIp) {
    EXPECT_EQ(hexToIp(0x0100007F), "127.0.0.1");     // bytes are stored backwards
    EXPECT_EQ(hexToIp(0x00000000), "0.0.0.0");
    EXPECT_EQ(hexToIp(0x0101A8C0), "192.168.1.1");
}

TEST(Connections, StateNames) {
    EXPECT_EQ(stateName(0x01), "ESTABLISHED");
    EXPECT_EQ(stateName(0x0A), "LISTEN");
    EXPECT_EQ(stateName(0x06), "TIME_WAIT");
    EXPECT_EQ(stateName(0xFF), "UNKNOWN");
}

TEST(Connections, ParseEndpointValid) {
    std::string addr;
    unsigned int port = 0;
    ASSERT_TRUE(parseEndpoint("0100007F:1F90", addr, port));
    EXPECT_EQ(addr, "127.0.0.1");
    EXPECT_EQ(port, 8080u);   // 0x1F90 = 8080; the "u" makes it unsigned like "port"
}

TEST(Connections, ParseEndpointRejectsGarbage) {
    std::string addr;
    unsigned int port = 0;
    EXPECT_FALSE(parseEndpoint("no-colon-here", addr, port));
    EXPECT_FALSE(parseEndpoint("ZZZZ:1F90", addr, port));   // not hex
    EXPECT_FALSE(parseEndpoint("", addr, port));
}

// ---------------------------------------------------------------------------
// Chip manager: the slot rules (including the bug we hit in enable())
// ---------------------------------------------------------------------------

TEST(ChipManager, EnableUsesSlotsAndRefusesWhenFull) {
    ChipManager chips(4);
    EXPECT_EQ(chips.enable("NETWORK SCAN"), ChipResult::Ok);    // costs 1 -> 1 used
    EXPECT_EQ(chips.enable("PROCESS WATCH"), ChipResult::Ok);   // costs 2 -> 3 used
    EXPECT_EQ(chips.enable("LOG STREAM"), ChipResult::Ok);      // costs 1 -> 4 used
    EXPECT_EQ(chips.usedSlots(), 4);

    // No room left: must be refused AND must stay switched off.
    EXPECT_EQ(chips.enable("DISK MONITOR"), ChipResult::NotEnoughSlots);
    EXPECT_FALSE(chips.isEnabled("DISK MONITOR"));
    EXPECT_EQ(chips.usedSlots(), 4);
}

TEST(ChipManager, EnablingTwiceIsReportedAndCostsNothingExtra) {
    ChipManager chips(4);
    EXPECT_EQ(chips.enable("PROCESS WATCH"), ChipResult::Ok);
    EXPECT_EQ(chips.enable("PROCESS WATCH"), ChipResult::AlreadyInState);
    EXPECT_EQ(chips.usedSlots(), 2);
}

TEST(ChipManager, UnknownChipIsNotFound) {
    ChipManager chips(4);
    EXPECT_EQ(chips.enable("NO SUCH CHIP"), ChipResult::NotFound);
    EXPECT_EQ(chips.disable("NO SUCH CHIP"), ChipResult::NotFound);
    EXPECT_FALSE(chips.isEnabled("NO SUCH CHIP"));
}

TEST(ChipManager, DisableFreesSlots) {
    ChipManager chips(4);
    chips.enable("PROCESS WATCH");
    EXPECT_EQ(chips.usedSlots(), 2);

    EXPECT_EQ(chips.disable("PROCESS WATCH"), ChipResult::Ok);
    EXPECT_EQ(chips.usedSlots(), 0);
    EXPECT_EQ(chips.disable("PROCESS WATCH"), ChipResult::AlreadyInState);
}

// ---------------------------------------------------------------------------
// Saved loadouts (JSON files)
// ---------------------------------------------------------------------------

TEST(Loadout, SaveThenLoadRestoresChips) {
    const std::string path = tempFile("pod042_test_loadout_roundtrip.json");
    std::filesystem::remove(path);   // start clean

    ChipManager saved(4);
    saved.enable("NETWORK SCAN");
    saved.enable("LOG STREAM");
    ASSERT_TRUE(saveLoadout(saved, path));   // ASSERT: nothing to check if saving failed

    ChipManager restored(4);
    restored.enable("PROCESS WATCH");        // must be switched OFF by the load
    ASSERT_TRUE(loadLoadout(restored, path));
    EXPECT_TRUE(restored.isEnabled("NETWORK SCAN"));
    EXPECT_TRUE(restored.isEnabled("LOG STREAM"));
    EXPECT_FALSE(restored.isEnabled("PROCESS WATCH"));
    EXPECT_FALSE(restored.isEnabled("DISK MONITOR"));

    std::filesystem::remove(path);
}

TEST(Loadout, MissingFileFailsGracefully) {
    const std::string path = tempFile("pod042_test_loadout_missing.json");
    std::filesystem::remove(path);
    ChipManager chips(4);
    EXPECT_FALSE(loadLoadout(chips, path));
}

TEST(Loadout, BrokenJsonIsRejected) {
    const std::string path = tempFile("pod042_test_loadout_broken.json");
    writeFile(path, "{ this is not json");
    ChipManager chips(4);
    EXPECT_FALSE(loadLoadout(chips, path));
    std::filesystem::remove(path);
}

TEST(Loadout, WrongShapeIsRejected) {
    const std::string path = tempFile("pod042_test_loadout_shape.json");
    ChipManager chips(4);

    // R"( ... )" is a raw string: backslashes and quotes inside need no escaping.
    writeFile(path, R"({"enabled": "oops"})");   // should be an array
    EXPECT_FALSE(loadLoadout(chips, path));

    writeFile(path, R"({"something": 1})");      // missing the "enabled" key
    EXPECT_FALSE(loadLoadout(chips, path));

    std::filesystem::remove(path);
}

TEST(Loadout, HandEditedFileCannotBreakTheSlotLimit) {
    const std::string path = tempFile("pod042_test_loadout_overbudget.json");
    // All four chips cost 5 slots in total, but the manager only has 4.
    writeFile(path,
        R"({"enabled": ["NETWORK SCAN", "PROCESS WATCH", "LOG STREAM", "DISK MONITOR"]})");

    ChipManager chips(4);
    ASSERT_TRUE(loadLoadout(chips, path));
    EXPECT_EQ(chips.usedSlots(), 4);                 // never above the budget
    EXPECT_FALSE(chips.isEnabled("DISK MONITOR"));   // the one that didn't fit

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// Operator name (profile.json)
// ---------------------------------------------------------------------------

TEST(Profile, SaveThenLoadRoundTrip) {
    const std::string path = tempFile("pod042_test_profile.json");
    std::filesystem::remove(path);

    ASSERT_TRUE(saveName("Shiro", path));
    std::string name = "unchanged";
    ASSERT_TRUE(loadName(name, path));
    EXPECT_EQ(name, "Shiro");

    std::filesystem::remove(path);
}

TEST(Profile, MissingFileLeavesNameUntouched) {
    const std::string path = tempFile("pod042_test_profile_missing.json");
    std::filesystem::remove(path);

    std::string name = "keep me";
    EXPECT_FALSE(loadName(name, path));
    EXPECT_EQ(name, "keep me");
}

TEST(Profile, EmptyNameCountsAsMissing) {
    const std::string path = tempFile("pod042_test_profile_empty.json");
    writeFile(path, R"({"name": ""})");

    std::string name = "keep me";
    EXPECT_FALSE(loadName(name, path));
    EXPECT_EQ(name, "keep me");

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// Event feed (ChangeDetector)
// ---------------------------------------------------------------------------

namespace {

// Builds a Connection with only the fields the detector looks at.
Connection makeConn(const std::string& state,
                    const std::string& localAddr, unsigned int localPort,
                    const std::string& remoteAddr, unsigned int remotePort,
                    unsigned long long inode = 0) {
    Connection c;
    c.state = state;
    c.localAddr = localAddr;
    c.localPort = localPort;
    c.remoteAddr = remoteAddr;
    c.remotePort = remotePort;
    c.inode = inode;
    return c;
}

ProcessInfo makeProc(int pid, const std::string& name) {
    ProcessInfo p;
    p.pid = pid;
    p.name = name;
    return p;
}

}  // namespace

TEST(Feed, EndpointFormatting) {
    EXPECT_EQ(endpoint("127.0.0.1", 8080), "127.0.0.1:8080");
    EXPECT_EQ(endpoint("::1", 8080), "[::1]:8080");   // IPv6 gets brackets
}

TEST(Feed, LoopbackDetection) {
    EXPECT_TRUE(isLoopbackAddress("127.0.0.1"));
    EXPECT_TRUE(isLoopbackAddress("127.1.2.3"));
    EXPECT_TRUE(isLoopbackAddress("::1"));
    EXPECT_TRUE(isLoopbackAddress("::ffff:127.0.0.1"));
    EXPECT_FALSE(isLoopbackAddress("10.0.0.5"));
    EXPECT_FALSE(isLoopbackAddress("1.127.0.0"));   // "127." must be at the START
    EXPECT_FALSE(isLoopbackAddress("::"));
}

TEST(Feed, FirstSampleIsOnlyABaseline) {
    ChangeDetector d;
    // Even a wide-open listener is not reported on the very first sample.
    auto changes = d.checkConnections({makeConn("LISTEN", "0.0.0.0", 8080, "0.0.0.0", 0)});
    EXPECT_TRUE(changes.empty());
}

TEST(Feed, NewListenerOnAllInterfacesIsAWarning) {
    ChangeDetector d;
    d.checkConnections({});   // baseline: nothing yet
    auto changes = d.checkConnections({makeConn("LISTEN", "0.0.0.0", 8080, "0.0.0.0", 0)});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].level, LogLevel::Warning);
    EXPECT_NE(changes[0].message.find("0.0.0.0:8080"), std::string::npos);
}

TEST(Feed, NewLoopbackListenerIsOnlyInfo) {
    ChangeDetector d;
    d.checkConnections({});
    auto changes = d.checkConnections({makeConn("LISTEN", "127.0.0.1", 41789, "0.0.0.0", 0)});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].level, LogLevel::Info);
}

TEST(Feed, ClosedListenerIsReported) {
    ChangeDetector d;
    d.checkConnections({});
    d.checkConnections({makeConn("LISTEN", "0.0.0.0", 8080, "0.0.0.0", 0)});
    auto changes = d.checkConnections({});   // the listener is gone
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_NE(changes[0].message.find("closed"), std::string::npos);
    EXPECT_NE(changes[0].message.find("0.0.0.0:8080"), std::string::npos);
}

TEST(Feed, UnchangedStateProducesNothing) {
    ChangeDetector d;
    std::vector<Connection> same = {
        makeConn("LISTEN", "0.0.0.0", 22, "0.0.0.0", 0),
        makeConn("ESTABLISHED", "10.0.0.2", 40000, "20.1.2.3", 443),
    };
    d.checkConnections(same);                       // baseline
    EXPECT_TRUE(d.checkConnections(same).empty());  // identical sample: no changes
}

TEST(Feed, NewOutboundConnectionIsReportedWithItsInode) {
    ChangeDetector d;
    d.checkConnections({});
    auto changes = d.checkConnections({
        makeConn("ESTABLISHED", "10.0.0.2", 40000, "20.233.83.145", 22, 777)});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_NE(changes[0].message.find("outbound"), std::string::npos);
    EXPECT_NE(changes[0].message.find("20.233.83.145:22"), std::string::npos);
    EXPECT_EQ(changes[0].inode, 777u);   // so the caller can look up the owning process
}

TEST(Feed, ManySocketsToTheSameServerCountOnce) {
    ChangeDetector d;
    d.checkConnections({});
    auto changes = d.checkConnections({
        makeConn("ESTABLISHED", "10.0.0.2", 40001, "20.1.2.3", 443),
        makeConn("ESTABLISHED", "10.0.0.2", 40002, "20.1.2.3", 443),   // same destination
        makeConn("ESTABLISHED", "10.0.0.2", 40003, "20.1.2.3", 443),
    });
    EXPECT_EQ(changes.size(), 1u);
}

TEST(Feed, LoopbackConnectionsAreIgnored) {
    ChangeDetector d;
    d.checkConnections({});
    auto changes = d.checkConnections({
        makeConn("ESTABLISHED", "127.0.0.1", 39215, "127.0.0.1", 42052)});
    EXPECT_TRUE(changes.empty());
}

TEST(Feed, ConnectionToOneOfOurListeningPortsIsInbound) {
    ChangeDetector d;
    Connection listener = makeConn("LISTEN", "0.0.0.0", 22, "0.0.0.0", 0);
    d.checkConnections({listener});   // baseline: just the listener

    auto changes = d.checkConnections({
        listener,
        makeConn("ESTABLISHED", "10.0.0.2", 22, "10.0.0.9", 50000)});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_NE(changes[0].message.find("inbound"), std::string::npos);
    EXPECT_NE(changes[0].message.find("10.0.0.9"), std::string::npos);
}

TEST(Feed, ResettingStartsANewBaseline) {
    ChangeDetector d;
    d.checkConnections({});
    d.resetConnections();   // as if the NETWORK SCAN chip was removed and reinstalled
    auto changes = d.checkConnections({makeConn("LISTEN", "0.0.0.0", 8080, "0.0.0.0", 0)});
    EXPECT_TRUE(changes.empty());   // no flood: this sample is a baseline again
}

TEST(Feed, NewProgramIsReportedOnlyTheFirstTime) {
    ChangeDetector d;
    d.checkProcesses({makeProc(1, "systemd"), makeProc(2, "bash")});   // baseline

    // The same programs again: nothing to report, even with different pids.
    EXPECT_TRUE(d.checkProcesses({makeProc(1, "systemd"), makeProc(50, "bash")}).empty());

    auto changes = d.checkProcesses({makeProc(1, "systemd"), makeProc(9, "nc")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_NE(changes[0].message.find("nc"), std::string::npos);
    EXPECT_NE(changes[0].message.find("pid 9"), std::string::npos);

    // "nc" is known now, so a second one is not news.
    EXPECT_TRUE(d.checkProcesses({makeProc(10, "nc")}).empty());
}

TEST(Feed, ResettingProcessesStartsANewBaseline) {
    ChangeDetector d;
    d.checkProcesses({makeProc(1, "bash")});
    d.resetProcesses();
    EXPECT_TRUE(d.checkProcesses({makeProc(1, "bash"), makeProc(2, "nc")}).empty());
}

TEST(Feed, LimitKeepsWarningsAndSummarizesTheRest) {
    std::vector<Change> changes;
    for (int i = 0; i < 6; ++i) {
        changes.push_back({LogLevel::Info, "info " + std::to_string(i), 0});
    }
    changes.push_back({LogLevel::Warning, "important", 0});   // added LAST on purpose

    limitChanges(changes, 3);

    ASSERT_EQ(changes.size(), 4u);                 // 3 kept + 1 summary line
    EXPECT_EQ(changes[0].message, "important");    // the warning survived the cut
    EXPECT_NE(changes[3].message.find("4 more"), std::string::npos);   // 7 total - 3 kept
}

TEST(Feed, LimitDoesNothingWhenUnderTheCap) {
    std::vector<Change> changes = {{LogLevel::Info, "one", 0}, {LogLevel::Info, "two", 0}};
    limitChanges(changes, 5);
    EXPECT_EQ(changes.size(), 2u);
}

TEST(Feed, ProcessNamesAreSanitized) {
    // A newline in a process name could forge a fake log line.
    EXPECT_EQ(sanitizeName("evil\n2026-01-01 [ERROR] fake"), "evil?2026-01-01 [ERROR] fake");
    EXPECT_EQ(sanitizeName("tab\there"), "tab?here");
    EXPECT_EQ(sanitizeName("plain-name_1"), "plain-name_1");
    EXPECT_EQ(sanitizeName("Şiro"), "Şiro");   // non-ASCII bytes are left alone
}

// ---------------------------------------------------------------------------
// Known programs: remembering names between runs
// ---------------------------------------------------------------------------

TEST(KnownPrograms, LoadedNamesAreNotReportedButNewOnesAreImmediately) {
    ChangeDetector d;
    d.loadKnownPrograms({"bash", "systemd"});

    // No separate baseline sample: the loaded list IS the baseline.
    auto changes = d.checkProcesses({makeProc(1, "systemd"), makeProc(2, "bash"), makeProc(9, "nc")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_NE(changes[0].message.find("nc"), std::string::npos);
}

TEST(KnownPrograms, EmptyLoadedListStillStartsWithAQuietBaseline) {
    ChangeDetector d;
    d.loadKnownPrograms({});   // like a first run: nothing saved yet
    EXPECT_TRUE(d.checkProcesses({makeProc(1, "bash"), makeProc(2, "nc")}).empty());
    EXPECT_EQ(d.knownPrograms().size(), 2u);   // both are remembered, for saving
}

TEST(KnownPrograms, ResetKeepsKnownNamesButRestartsTheBaseline) {
    ChangeDetector d;
    d.loadKnownPrograms({"bash"});
    d.resetProcesses();   // as if the PROCESS WATCH chip was removed and reinstalled

    // The first sample after the reset is quiet, even for a name not in the list...
    EXPECT_TRUE(d.checkProcesses({makeProc(1, "bash"), makeProc(2, "nc")}).empty());
    // ...and everything stays known afterwards.
    EXPECT_EQ(d.knownPrograms().count("bash"), 1u);
    EXPECT_EQ(d.knownPrograms().count("nc"), 1u);
    EXPECT_TRUE(d.checkProcesses({makeProc(3, "nc")}).empty());
    EXPECT_EQ(d.checkProcesses({makeProc(4, "brandnew")}).size(), 1u);
}

TEST(KnownPrograms, ListHasACeilingAndWarnsOnlyOnce) {
    std::set<std::string> full;
    for (std::size_t i = 0; i < kMaxKnownPrograms; ++i) {
        full.insert("prog" + std::to_string(i));
    }
    ChangeDetector d;
    d.loadKnownPrograms(full);

    auto changes = d.checkProcesses({makeProc(1, "overflow1")});
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].level, LogLevel::Warning);
    EXPECT_NE(changes[0].message.find("full"), std::string::npos);
    EXPECT_EQ(d.knownPrograms().size(), kMaxKnownPrograms);   // the new name was NOT added

    // A second unknown name: no second warning, and still not added.
    EXPECT_TRUE(d.checkProcesses({makeProc(2, "overflow2")}).empty());
    EXPECT_EQ(d.knownPrograms().size(), kMaxKnownPrograms);
}

TEST(KnownPrograms, SaveThenLoadRoundTrip) {
    const std::string path = tempFile("pod042_test_known.json");
    std::filesystem::remove(path);

    ASSERT_TRUE(saveKnownPrograms({"bash", "nc", "sshd"}, path));
    std::set<std::string> loaded = loadKnownPrograms(path);
    EXPECT_EQ(loaded, (std::set<std::string>{"bash", "nc", "sshd"}));

    // The temporary file used for the safe write must not be left behind.
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));

    std::filesystem::remove(path);
}

TEST(KnownPrograms, MissingFileGivesAnEmptyList) {
    const std::string path = tempFile("pod042_test_known_missing.json");
    std::filesystem::remove(path);
    EXPECT_TRUE(loadKnownPrograms(path).empty());
}

TEST(KnownPrograms, BrokenOrWrongShapeFilesGiveAnEmptyList) {
    const std::string path = tempFile("pod042_test_known_broken.json");

    writeFile(path, "{ not json");
    EXPECT_TRUE(loadKnownPrograms(path).empty());

    writeFile(path, R"({"programs": "oops"})");   // should be an array
    EXPECT_TRUE(loadKnownPrograms(path).empty());

    writeFile(path, R"(["bash", "nc"])");         // an array, but not an object with "programs"
    EXPECT_TRUE(loadKnownPrograms(path).empty());

    std::filesystem::remove(path);
}

TEST(KnownPrograms, LoadSkipsNonStringsAndCleansNames) {
    const std::string path = tempFile("pod042_test_known_mixed.json");
    // A hand-edited file: a number, null, and a name containing a newline escape.
    writeFile(path, R"({"programs": ["bash", 42, null, "evil\nname"]})");

    std::set<std::string> loaded = loadKnownPrograms(path);
    EXPECT_EQ(loaded.count("bash"), 1u);
    EXPECT_EQ(loaded.count("evil?name"), 1u);   // the newline became '?'
    EXPECT_EQ(loaded.size(), 2u);               // 42 and null were skipped

    std::filesystem::remove(path);
}