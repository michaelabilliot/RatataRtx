#include <vector>
#include <string>
#include <sstream>

std::vector<int> StringPatternToBytes(const std::string& pattern)
{
    std::vector<int> bytes;
    std::istringstream stream(pattern);
    std::string byte;

    while (stream >> byte) {
        if (byte == "??")
            bytes.push_back(-1);
        else
            bytes.push_back(std::stoi(byte, nullptr, 16));
    }

    return bytes;
}

uintptr_t FindSignature(uintptr_t base, size_t size, const std::string& pattern)
{
    auto bytes = StringPatternToBytes(pattern);
    size_t patternSize = bytes.size();

    for (size_t i = 0; i <= size - patternSize; i++) {
        bool found = true;

        for (size_t j = 0; j < patternSize; j++) {
            if (bytes[j] != -1 && bytes[j] != *(uint8_t*)(base + i + j)) {
                found = false;
                break;
            }
        }

        if (found)
            return base + i;
    }

    return 0;
}