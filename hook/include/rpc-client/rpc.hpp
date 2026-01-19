#pragma once
#include <Windows.h>

bool InitRPC();
void RunRPC();
DWORD WINAPI RPCThreadMain(LPVOID);