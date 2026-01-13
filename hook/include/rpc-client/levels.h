#pragma once
#include <Windows.h>
#include <cstdint>
#include <initializer_list>

extern DWORD levelIdBaseAddr;
extern DWORD playerObjectsAddr;
extern DWORD getIDAddr;

struct Level {
    char key;
    const char* name;
    const char* imageName;
};

void initRat();
Level getLevel();
const char* getCharName();