#pragma once
#include <vector>
#include <string>
#include <sstream>

std::vector<int> StringPatternToBytes(const std::string& pattern);
uintptr_t FindSignature(uintptr_t base, size_t size, const std::string& pattern);