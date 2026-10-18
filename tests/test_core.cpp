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