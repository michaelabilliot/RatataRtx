#pragma once
#include <vector>
#include <string>
#include <sstream>
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <iostream>

std::vector<int> StringPatternToBytes(const std::string& Sig);

template<size_t N>
struct Signature {
    std::array<uint8_t, N> bytes;
    std::array<char, N> mask;
};

template <size_t N>
uintptr_t FindSignature(uintptr_t base, size_t size, const Signature<N>& sig) {
    if (N > size)
        return 0;

    const auto* memory = reinterpret_cast<const std::uint8_t*>(base);

    for (size_t i = 0; i <= size - N; i++) {
        bool found = true;

        for (size_t j = 0; j < N; j++) {
            if (sig.mask[j] == 'x' && sig.bytes[j] != memory[i + j]) {
                found = false;
                break;
            }
        }

        if (found)
            return base + i;
    }
    
    return 0;
}