#include <Windows.H>
#include "hook.hpp"

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID lpReserved) {
    switch (dwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, HookMain, hModule, 0, nullptr);
            break;
    }
    return TRUE;
}
