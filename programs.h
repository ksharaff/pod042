#pragma once
#include <set>
#include <string>


std::set<std::string> loadKnownPrograms(const std::string& path);

bool saveKnownPrograms(const std::set<std::string>& names, const std::string& path);