#pragma once
#include <Windows.h>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <type_traits>

template<typename T, typename Ptr>
T* follow_pointer_chain(Ptr base, std::initializer_list<uintptr_t> offsets, bool applyOffsetAfterLastDeref = true) {
    if (!base) return nullptr;
    char* addr = reinterpret_cast<char*>(base);

    size_t i = 0;
    for (auto offset : offsets) {
        if (!addr) return nullptr;

        addr = *reinterpret_cast<char**>(addr);
        if (!addr) return nullptr;

        if (i < offsets.size() - 1 || applyOffsetAfterLastDeref) {
            addr += offset;
        }
        i++;
    }
    return reinterpret_cast<T*>(addr);
}

uintptr_t ReadPtr32(uintptr_t address);