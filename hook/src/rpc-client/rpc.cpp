#include <Windows.h>
#include "rpc-client/discord_manager.h"
#include "rpc-client/rpc.h"

constexpr DWORD UpdateFreq = 1000;

DWORD WINAPI InitRPC(LPVOID lpParameter) {
	DiscordMan_Startup();

	while (true) {
		DiscordMan_Update();
		Sleep(UpdateFreq);
	}

	return 0;
}
