#include <Windows.h>
#include <stdio.h>
#include "rpc-client/discord_manager.hpp"

constexpr char rpcLibName[] = "discord-rpc.dll";
constexpr double updateFreq = 5.0;

bool InitRPC() {
	while (true) {
		HMODULE hDiscord = GetModuleHandleA(rpcLibName);
		if (!hDiscord)
			hDiscord = LoadLibraryA(rpcLibName);

		if (hDiscord)
			return true;

		char buffer[256];
		sprintf_s(buffer, sizeof(buffer),
			"Unable to initialize Discord Rich Presence.\nMake sure to extract %s into the game directory.", rpcLibName
		);

		int choice = MessageBoxA(nullptr, buffer, "RatataR", MB_RETRYCANCEL | MB_ICONERROR);
		if (choice != IDRETRY)
			return false;
	}
}

void RunRPC() {
	DiscordMan_Startup();

	DWORD UpdateFreqMs = static_cast<DWORD>(updateFreq * 1000.0);
	while (true) {
		DiscordMan_Update();
		Sleep(UpdateFreqMs);
	}
}

DWORD WINAPI RPCThreadMain(LPVOID) {
	RunRPC();
	return 0;
}