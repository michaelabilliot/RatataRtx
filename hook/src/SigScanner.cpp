#include <vector>
#include <string>
#include <sstream>

std::vector<int> StringPatternToBytes(const std::string& Sig)
{
    std::vector<int> bytes;
    std::istringstream stream(Sig);
    std::string byte;

    while (stream >> byte) {
        if (byte == "??")
            bytes.push_back(-1);
        else
            bytes.push_back(std::stoi(byte, nullptr, 16));
    }

    return bytes;
}