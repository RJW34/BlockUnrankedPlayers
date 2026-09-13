// dllmain.cpp - drop-in dinput8.dll for Slippi Dolphin that adds a connect-code blocklist.
//
// How it works (see BlocklistHookCore.h for the logic):
//   * Dolphin imports dinput8.dll. A copy of this DLL in Dolphin's folder is
//     loaded instead of the system one, and forwards every DirectInput call to
//     the real C:\Windows\System32\dinput8.dll. Game input is untouched.
//   * At load time we patch Dolphin's import table entries for WSARecvFrom and
//     WSASendTo (the two Winsock calls ENet uses).
//   * Traffic with the matchmaking server (port 43113) is read to learn who we
//     were matched with. Traffic with a blocked player is silently dropped, so
//     Dolphin's own connection timeout kicks in and it searches again.
//
// Config: User\Slippi\blocklist.json next to user.json. Log: blocklist.log there.
//
// Build (MinGW-w64):
//   g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++
//       -I../dolphin -I<nlohmann include dir> -o dinput8.dll dllmain.cpp -lws2_32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <unknwn.h>

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#include "BlocklistHookCore.h"

#define HOOK_VERSION "1.0.0"

namespace
{
// ------------------------------------------------------------------------- //
// Paths, config, logging
// ------------------------------------------------------------------------- //
std::string g_exe_dir;
std::string g_config_path;
std::string g_log_path;
std::mutex g_log_mutex;
FILETIME g_config_mtime = {0, 0};
bool g_config_ever_loaded = false;
SlippiHook::BlocklistHook g_hook;

std::string DirName(const std::string& path)
{
	size_t slash = path.find_last_of("\\/");
	return slash == std::string::npos ? "." : path.substr(0, slash);
}

bool FileExists(const std::string& path)
{
	DWORD attr = GetFileAttributesA(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void LogLine(const std::string& msg)
{
	std::lock_guard<std::mutex> lock(g_log_mutex);
	if (g_log_path.empty())
		return;
	FILE* f = std::fopen(g_log_path.c_str(), "a");
	if (!f)
		return;
	std::time_t t = std::time(nullptr);
	std::tm tm;
	localtime_s(&tm, &t);
	char stamp[32];
	std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
	std::fprintf(f, "%s  %s\n", stamp, msg.c_str());
	std::fclose(f);
}

void TrimLogIfHuge()
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	if (GetFileAttributesExA(g_log_path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeHigh == 0 &&
	    fad.nFileSizeLow > 2 * 1024 * 1024)
	{
		DeleteFileA(g_log_path.c_str());
	}
}

// Dolphin runs in "portable" mode from the Launcher: User\ lives next to the exe.
// Fall back to the Launcher's AppData copy, then to a file next to the exe.
void ResolvePaths()
{
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	g_exe_dir = DirName(exe);

	std::string candidates[4];
	int n = 0;
	candidates[n++] = g_exe_dir + "\\User\\Slippi\\" + SlippiBlocklist::FILE_NAME;
	char appdata[MAX_PATH] = "";
	if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0)
	{
		candidates[n++] = std::string(appdata) + "\\Slippi Launcher\\netplay\\User\\Slippi\\" + SlippiBlocklist::FILE_NAME;
		candidates[n++] =
		    std::string(appdata) + "\\Slippi Launcher\\netplay-beta\\User\\Slippi\\" + SlippiBlocklist::FILE_NAME;
	}
	candidates[n++] = g_exe_dir + "\\" + SlippiBlocklist::FILE_NAME;

	g_config_path = candidates[0];
	for (int i = 0; i < n; i++)
	{
		if (FileExists(candidates[i]))
		{
			g_config_path = candidates[i];
			break;
		}
	}
	g_log_path = DirName(g_config_path) + "\\blocklist.log";
}

bool ReadWholeFile(const std::string& path, std::string* out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buf[4096];
	size_t got;
	out->clear();
	while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, got);
	std::fclose(f);
	return true;
}

// (Re)load blocklist.json if it changed since last time. Cheap; called on the
// matchmaking thread each time Dolphin talks to the server.
void ReloadConfigIfChanged()
{
	WIN32_FILE_ATTRIBUTE_DATA fad;
	bool exists = GetFileAttributesExA(g_config_path.c_str(), GetFileExInfoStandard, &fad) != 0;
	if (!exists)
	{
		if (g_config_ever_loaded || !g_config_ever_loaded)
		{
			SlippiBlocklist::Config empty;
			if (!g_config_ever_loaded || g_config_mtime.dwLowDateTime != 0)
				LogLine("no " + g_config_path + " found; nothing will be filtered");
			g_hook.SetConfig(empty);
			g_config_mtime = FILETIME{0, 0};
			g_config_ever_loaded = true;
		}
		return;
	}
	if (g_config_ever_loaded && fad.ftLastWriteTime.dwLowDateTime == g_config_mtime.dwLowDateTime &&
	    fad.ftLastWriteTime.dwHighDateTime == g_config_mtime.dwHighDateTime)
		return;

	std::string text;
	if (!ReadWholeFile(g_config_path, &text))
	{
		LogLine("could not read " + g_config_path);
		return;
	}
	std::string err;
	SlippiBlocklist::Config cfg = SlippiBlocklist::Parse(text, &err);
	if (!err.empty())
		LogLine("ERROR parsing " + g_config_path + ": " + err + " (blocklist disabled until fixed)");
	for (const auto& w : cfg.warnings)
		LogLine("warning: " + w);
	LogLine("loaded " + g_config_path + ": " + SlippiBlocklist::Summary(cfg));
	g_hook.SetConfig(cfg);
	g_config_mtime = fad.ftLastWriteTime;
	g_config_ever_loaded = true;
}

// ------------------------------------------------------------------------- //
// Winsock hooks
// ------------------------------------------------------------------------- //
typedef int(WSAAPI* WSARecvFrom_t)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int(WSAAPI* WSASendTo_t)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);

WSARecvFrom_t g_real_WSARecvFrom = nullptr;
WSASendTo_t g_real_WSASendTo = nullptr;

uint64_t NowMs()
{
	return (uint64_t)GetTickCount64();
}

bool IsMmServer(const sockaddr* sa, int len)
{
	if (!sa || len < (int)sizeof(sockaddr_in) || sa->sa_family != AF_INET)
		return false;
	return ntohs(((const sockaddr_in*)sa)->sin_port) == SlippiHook::MM_PORT;
}

int WSAAPI Hook_WSARecvFrom(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD received, LPDWORD flags, sockaddr* from,
                            LPINT fromlen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine)
{
	int r = g_real_WSARecvFrom(s, buffers, count, received, flags, from, fromlen, overlapped, routine);
	if (r != 0 || overlapped || !buffers || count < 1 || !received || !from || !fromlen ||
	    *fromlen < (int)sizeof(sockaddr_in) || from->sa_family != AF_INET)
		return r;

	const sockaddr_in* sin = (const sockaddr_in*)from;
	uint32_t ip = sin->sin_addr.s_addr;
	uint64_t now = NowMs();

	if (ntohs(sin->sin_port) == SlippiHook::MM_PORT)
	{
		ReloadConfigIfChanged();
		g_hook.OnMmIncoming((const uint8_t*)buffers[0].buf, *received, now);
		return r;
	}

	if (g_hook.ShouldDrop(ip, now))
	{
		// Pretend nothing arrived: ENet treats a 0-byte receive as "no packet".
		*received = 0;
		return 0;
	}
	return r;
}

int WSAAPI Hook_WSASendTo(SOCKET s, LPWSABUF buffers, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to,
                          int tolen, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine)
{
	if (to && tolen >= (int)sizeof(sockaddr_in) && to->sa_family == AF_INET && buffers && count >= 1)
	{
		const sockaddr_in* sin = (const sockaddr_in*)to;
		uint64_t now = NowMs();
		if (IsMmServer(to, tolen))
		{
			ReloadConfigIfChanged();
			g_hook.OnMmOutgoing((const uint8_t*)buffers[0].buf, buffers[0].len, now);
		}
		else if (g_hook.ShouldDrop(sin->sin_addr.s_addr, now))
		{
			// Swallow it but report success so ENet keeps retrying until its own timeout.
			DWORD total = 0;
			for (DWORD i = 0; i < count; i++)
				total += buffers[i].len;
			if (sent)
				*sent = total;
			return 0;
		}
	}
	return g_real_WSASendTo(s, buffers, count, sent, flags, to, tolen, overlapped, routine);
}

// Classic import-address-table patch on the main executable.
bool PatchImport(HMODULE module, const char* dll_name, const char* func_name, void* hook, void** original)
{
	uint8_t* base = (uint8_t*)module;
	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress)
		return false;

	HMODULE real_dll = GetModuleHandleA(dll_name);
	void* real_fn = real_dll ? (void*)GetProcAddress(real_dll, func_name) : nullptr;

	for (IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); desc->Name; desc++)
	{
		const char* name = (const char*)(base + desc->Name);
		if (_stricmp(name, dll_name) != 0)
			continue;
		IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
		IMAGE_THUNK_DATA* names = desc->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk) : nullptr;
		for (size_t i = 0; thunk[i].u1.Function; i++)
		{
			bool match = false;
			if (names && !(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG))
			{
				IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
				match = std::strcmp((const char*)ibn->Name, func_name) == 0;
			}
			else if (real_fn)
			{
				match = (void*)thunk[i].u1.Function == real_fn;
			}
			if (!match)
				continue;
			DWORD old = 0;
			if (!VirtualProtect(&thunk[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old))
				return false;
			*original = (void*)thunk[i].u1.Function;
			thunk[i].u1.Function = (ULONG_PTR)hook;
			VirtualProtect(&thunk[i].u1.Function, sizeof(void*), old, &old);
			return true;
		}
	}
	return false;
}

void InstallHooks()
{
	ResolvePaths();
	TrimLogIfHuge();
	LogLine(std::string("--- blocklist hook v" HOOK_VERSION " loaded into ") + g_exe_dir + " ---");

	g_hook.log = [](const std::string& s) { LogLine(s); };
	g_hook.on_skip = [](const SlippiHook::SkipEvent& ev) {
		if (ev.beep)
			MessageBeep(MB_ICONWARNING);
	};

	HMODULE exe = GetModuleHandleA(nullptr);
	bool a = PatchImport(exe, "WS2_32.dll", "WSARecvFrom", (void*)&Hook_WSARecvFrom, (void**)&g_real_WSARecvFrom);
	bool b = PatchImport(exe, "WS2_32.dll", "WSASendTo", (void*)&Hook_WSASendTo, (void**)&g_real_WSASendTo);
	LogLine(std::string("hooks: WSARecvFrom=") + (a ? "ok" : "FAILED") + " WSASendTo=" + (b ? "ok" : "FAILED"));
	if (a && b)
		ReloadConfigIfChanged();
	else
		LogLine("the executable does not import the expected Winsock functions; blocklist inactive");
}

// ------------------------------------------------------------------------- //
// dinput8.dll forwarding
// ------------------------------------------------------------------------- //
HMODULE g_real_dinput8 = nullptr;

void* RealDInput(const char* name)
{
	if (!g_real_dinput8)
	{
		char sys[MAX_PATH];
		if (GetSystemDirectoryA(sys, MAX_PATH) == 0)
			return nullptr;
		std::string path = std::string(sys) + "\\dinput8.dll";
		g_real_dinput8 = LoadLibraryA(path.c_str());
		if (!g_real_dinput8)
			return nullptr;
	}
	return (void*)GetProcAddress(g_real_dinput8, name);
}
}  // namespace

extern "C"
{
	__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out,
	                                                        LPUNKNOWN outer)
	{
		typedef HRESULT(WINAPI * Fn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		static Fn fn = (Fn)RealDInput("DirectInput8Create");
		if (!fn)
			return E_FAIL;
		return fn(hinst, version, riid, out, outer);
	}

	__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow()
	{
		typedef HRESULT(WINAPI * Fn)();
		static Fn fn = (Fn)RealDInput("DllCanUnloadNow");
		return fn ? fn() : S_FALSE;
	}

	__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* out)
	{
		typedef HRESULT(WINAPI * Fn)(REFCLSID, REFIID, LPVOID*);
		static Fn fn = (Fn)RealDInput("DllGetClassObject");
		return fn ? fn(rclsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
	}

	__declspec(dllexport) HRESULT WINAPI DllRegisterServer()
	{
		typedef HRESULT(WINAPI * Fn)();
		static Fn fn = (Fn)RealDInput("DllRegisterServer");
		return fn ? fn() : E_FAIL;
	}

	__declspec(dllexport) HRESULT WINAPI DllUnregisterServer()
	{
		typedef HRESULT(WINAPI * Fn)();
		static Fn fn = (Fn)RealDInput("DllUnregisterServer");
		return fn ? fn() : E_FAIL;
	}

	__declspec(dllexport) const void* WINAPI GetdfDIJoystick()
	{
		typedef const void*(WINAPI * Fn)();
		static Fn fn = (Fn)RealDInput("GetdfDIJoystick");
		return fn ? fn() : nullptr;
	}

	// Introspection for the test harness and for curious users.
	__declspec(dllexport) const char* WINAPI SlippiBlocklist_Version() { return HOOK_VERSION; }
	__declspec(dllexport) const char* WINAPI SlippiBlocklist_ConfigPath() { return g_config_path.c_str(); }
	__declspec(dllexport) unsigned long long WINAPI SlippiBlocklist_SkipCount() { return g_hook.SkipCount(); }
	__declspec(dllexport) unsigned long long WINAPI SlippiBlocklist_DroppedDatagrams()
	{
		return g_hook.DroppedDatagrams();
	}
	__declspec(dllexport) int WINAPI SlippiBlocklist_HooksInstalled()
	{
		return (g_real_WSARecvFrom && g_real_WSASendTo) ? 1 : 0;
	}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(instance);
		InstallHooks();
	}
	return TRUE;
}
