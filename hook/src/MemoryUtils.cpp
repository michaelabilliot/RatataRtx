#include <cstdint>
#include <initializer_list>
#include <optional>
#include <type_traits>

uintptr_t ReadPtr32(uintptr_t address) {
    return *reinterpret_cast<uintptr_t*>(address);
}