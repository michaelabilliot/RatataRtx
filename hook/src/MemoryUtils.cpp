#include <cstdint>
#include <initializer_list>
#include <optional>
#include <type_traits>
#include "MemoryUtils.h"

uintptr_t ReadPtr32(uintptr_t address) {
    return *reinterpret_cast<uintptr_t*>(address);
}