#pragma once   // stops this header being pasted in twice by accident

// Declaration only: "this struct and this function exist".
struct MemInfo {
    long totalKb = 0;
    long availableKb = 0;
};

MemInfo readMemInfo();