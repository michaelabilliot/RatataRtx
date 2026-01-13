#include <Windows.h>
#include <iostream>
#include <string>
#include <array>

std::string GetBaseDirectory(void) {
	char path[MAX_PATH]{};
	if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
		return {};

	std::string p = path;
	return p.substr(0, p.find_last_of("\\/"));
}

bool LaunchSuspendedProcess(const std::string& exePath, PROCESS_INFORMATION& pi) {
	STARTUPINFOA si{};
	si.cb = sizeof(si);

	return CreateProcessA(
		exePath.c_str(),
		nullptr,
		nullptr,
		nullptr,
		FALSE,
		CREATE_SUSPENDED,
		nullptr,
		nullptr,
		&si,
		&pi
	);
}

bool InjectDLL(HANDLE process, const std::string& dllPath) {
	LPVOID remoteMem = VirtualAllocEx(
		process,
		nullptr,
		dllPath.size() + 1,
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READWRITE
	);

	if (!remoteMem)
		return false;

	if (!WriteProcessMemory(
		process,
		remoteMem,
		dllPath.c_str(),
		dllPath.size() + 1,
		nullptr)) {
		VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
		return false;
	}

	HANDLE thread = CreateRemoteThread(
		process,
		nullptr,
		0,
		reinterpret_cast<LPTHREAD_START_ROUTINE>(LoadLibraryA),
		remoteMem,
		0,
		nullptr
	);

	if (!thread) {
		VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
		return false;
	}

	WaitForSingleObject(thread, INFINITE);
	VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
	CloseHandle(thread);
	
	return true;
}

bool TryOpenEventWithRetries(HANDLE& evt, int maxRetries = 10, int sleepMs = 100) {
	for (int attempt = 0; attempt < maxRetries; attempt++) {
		evt = OpenEventA(SYNCHRONIZE, FALSE, "RatataR_Patched");
		if (evt != nullptr)
			return true;

		Sleep(sleepMs);
	}

	evt = nullptr;
	return false;
}

bool WaitForPatchEventOrExit(HANDLE process) {
	HANDLE evt;
	if (!TryOpenEventWithRetries(evt))
		return false;

	DWORD result = WaitForMultipleObjects(
		2,
		std::array<HANDLE, 2>{ evt, process }.data(),
		FALSE,
		INFINITE
	);

	CloseHandle(evt);
	return result == WAIT_OBJECT_0;
}

int InjectAndRunHook(void) {
	const char* dllName = "hook.dll";
	const char* gameName = "overlay.exe";

	std::string baseDir = GetBaseDirectory();

	std::string exePath = baseDir + "\\" + gameName;
	std::string dllPath = baseDir + "\\" + dllName;

	PROCESS_INFORMATION pi{};
	if (!LaunchSuspendedProcess(exePath, pi)) {
		MessageBoxA(nullptr, "Failed to start overlay.exe", "Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	HANDLE process = pi.hProcess;
	HANDLE mainThread = pi.hThread;

	if (!InjectDLL(process, dllPath)) {
		MessageBoxA(nullptr, "Failed to inject hook.dll into the target process.", "Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	if (!WaitForPatchEventOrExit(process)) {
		MessageBoxA(nullptr, "Unexpect error.", "Error", MB_OK | MB_ICONERROR);
		TerminateProcess(process, 0);
		CloseHandle(process);
		CloseHandle(mainThread);
		return 1;
	}

	ResumeThread(mainThread);

	CloseHandle(process);
	CloseHandle(mainThread);

	if (HWND hwnd = FindWindowA(nullptr, "Ratatouille")) {
		SetForegroundWindow(hwnd);
	}

	return 0;
}