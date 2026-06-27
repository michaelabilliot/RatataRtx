#include <Windows.h>
#include <iostream>
#include <string>
#include <array>
#include <cstring>

std::string GetBaseDirectory() {
	char path[MAX_PATH]{};
	if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
		return {};

	std::string p = path;
	return p.substr(0, p.find_last_of("\\/"));
}

std::string AppendPath(const std::string& base, const char* leaf) {
	if (base.empty())
		return leaf;

	const char last = base.back();
	if (last == '\\' || last == '/')
		return base + leaf;

	return base + "\\" + leaf;
}

std::string GetEnvironmentPath(const char* name) {
	char path[MAX_PATH]{};
	const DWORD length = GetEnvironmentVariableA(name, path, sizeof(path));
	if (length == 0 || length >= sizeof(path))
		return {};

	return path;
}

std::string GetTempDirectory() {
	char path[MAX_PATH]{};
	const DWORD length = GetTempPathA(sizeof(path), path);
	if (length == 0 || length >= sizeof(path))
		return {};

	return path;
}

bool EnsureDirectoryExists(const std::string& path) {
	if (path.empty())
		return false;

	if (CreateDirectoryA(path.c_str(), nullptr))
		return true;

	return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string GetWritableRatataRDirectory() {
	std::string base = GetEnvironmentPath("LOCALAPPDATA");
	if (base.empty())
		base = GetTempDirectory();

	if (base.empty())
		return {};

	const std::string dir = AppendPath(base, "RatataR");
	return EnsureDirectoryExists(dir) ? dir : std::string{};
}

bool WriteTextFile(const std::string& path, const char* content) {
	HANDLE file = CreateFileA(
		path.c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr
	);

	if (file == INVALID_HANDLE_VALUE)
		return false;

	const DWORD bytesToWrite = static_cast<DWORD>(std::strlen(content));
	DWORD bytesWritten = 0;
	const BOOL ok = WriteFile(file, content, bytesToWrite, &bytesWritten, nullptr);
	CloseHandle(file);
	return ok && bytesWritten == bytesToWrite;
}

bool EnsureDxvkConfig(std::string& configPath) {
	const std::string dir = GetWritableRatataRDirectory();
	if (dir.empty())
		return false;

	configPath = AppendPath(dir, "dxvk-ratatouille.conf");
	constexpr char content[] =
		"d3d9.psShaderModel = 2\r\n"
		"d3d9.vsShaderModel = 2\r\n"
		"rtx.useUnusedRenderstates = True\r\n"
		"rtx.vertexColorStrength = 1.0\r\n"
		"rtx.useVertexCapture = False\r\n"
		"rtx.useVertexCapturedNormals = False\r\n"
		"rtx.useWorldMatricesForShaders = False\r\n"
		"rtx.orthographicIsUI = True\r\n";

	return WriteTextFile(configPath, content);
}

bool FileExists(const std::string& path) {
	const DWORD attributes = GetFileAttributesA(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool LaunchSuspendedProcess(const std::string& exePath, const std::string& workingDirectory, PROCESS_INFORMATION& pi) {
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
		workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
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

int InjectAndRunHook() {
	constexpr char dllName[] = "hook.dll";
	constexpr char gameName[] = "overlay.exe";

	const std::string baseDir = GetBaseDirectory();
	constexpr char installedGameDir[] = "C:\\Program Files (x86)\\THQ\\Disney-Pixar\\Ratatouille\\Rat";
	std::string gameDir = baseDir;

	if (!FileExists(AppendPath(gameDir, gameName)) && FileExists(AppendPath(installedGameDir, gameName))) {
		gameDir = installedGameDir;
	}

	const std::string exePath = AppendPath(gameDir, gameName);
	std::string dllPath = AppendPath(gameDir, dllName);
	if (!FileExists(dllPath)) {
		dllPath = AppendPath(baseDir, dllName);
	}

	std::string dxvkConfigPath;
	if (EnsureDxvkConfig(dxvkConfigPath)) {
		SetEnvironmentVariableA("DXVK_CONFIG_FILE", dxvkConfigPath.c_str());
	}

	PROCESS_INFORMATION pi{};
	if (!LaunchSuspendedProcess(exePath, gameDir, pi)) {
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

		if (DWORD exitCode; GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE)
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
