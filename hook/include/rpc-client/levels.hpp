#pragma once
#include <Windows.h>
#include <cstdint>
#include <initializer_list>

inline uintptr_t levelIdBase = 0;
inline uintptr_t playerObjectsBase = 0;
inline uintptr_t getIDBase = 0;

struct Level {
    char key;
    const char* name;
    const char* imageName;
};

void initRat();
Level getLevel();
const char* getCharName();