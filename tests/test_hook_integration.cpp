// In-process integration test for the built dinput8.dll.
//
// This exe imports WSARecvFrom/WSASendTo statically (like Dolphin), writes a
// blocklist.json where the DLL will look for it (User\Slippi next to the exe),
// loads the DLL, and then plays both sides of a matchmaking exchange over UDP
// loopback:
//   * a fake matchmaking server on 127.0.0.1:43113
//   * "Dolphin" on 127.0.0.1:41000
//   * a fake blocked opponent on 127.0.0.1:43114
// and checks that traffic with the opponent is dropped only after the server
// assigned them, and only for the configured modes.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <unknwn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                             \
	do                                                                                          \
	{                                                                                           \
		g_checks++;                                                                             \
		if (!(cond))                                                                            \
		{                                                                                       \
			g_failures++;                                                                       \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
		}                                                                                       \
	} while (0)

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
static void put32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(x >> 24); v.push_back((x >> 16) & 0xFF); v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}

static std::vector<uint8_t> Reliable(const std::string& data, uint16_t seq)
{
	std::vector<uint8_t> v;
	put16(v, 0x8001);  // peer 1, sent-time flag
	put16(v, 0);
	v.push_back(6 | 0x80);
	v.push_back(0);
	put16(v, seq);
	put16(v, (uint16_t)data.size());
	v.insert(v.end(), data.begin(), data.end());
	return v;
}

static std::vector<std::vector<uint8_t>> Fragments(const std::string& data, uint16_t start_seq, uint32_t count)
{
	std::vector<std::vector<uint8_t>> out;
	uint32_t total = (uint32_t)data.size(), frag_len = (total + count - 1) / count;
	for (uint32_t n = 0; n < count; n++)
	{
		uint32_t off = n * frag_len, len = off + frag_len > total ? total - off : frag_len;
		std::vector<uint8_t> v;
		put16(v, 0x8001);
		put16(v, 0);
		v.push_back(8 | 0x80);
		v.push_back(0);
		put16(v, (uint16_t)(start_seq + n));
		put16(v, start_seq);
		put16(v, (uint16_t)len);
		put32(v, count);
		put32(v, n);
		put32(v, total);
		put32(v, off);
		v.insert(v.end(), data.begin() + off, data.begin() + off + len);
		out.push_back(v);
	}
	return out;
}

static SOCKET Bind(const char* ip, uint16_t port)
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(ip);
	if (bind(s, (sockaddr*)&a, sizeof(a)) != 0)
	{
		std::fprintf(stderr, "bind %u failed: %d\n", port, WSAGetLastError());
		std::exit(2);
	}
	DWORD timeout = 300;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
	return s;
}

static sockaddr_in Addr(const char* ip, uint16_t port)
{
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(ip);
	return a;
}

static const char* SERVER_IP = "127.0.0.1";    // fake mm.slippi.gg
static const char* DOLPHIN_IP = "127.0.0.1";   // us
static const char* OPPONENT_IP = "127.0.0.2";  // blocked player
static const char* OTHER_IP = "127.0.0.3";     // someone else

// Send exactly the way ENet does on Windows.
static int Send(SOCKET s, const std::vector<uint8_t>& data, const char* to_ip, uint16_t to_port)
{
	WSABUF buf;
	buf.buf = (char*)data.data();
	buf.len = (ULONG)data.size();
	DWORD sent = 0;
	sockaddr_in to = Addr(to_ip, to_port);
	if (WSASendTo(s, &buf, 1, &sent, 0, (sockaddr*)&to, sizeof(to), nullptr, nullptr) != 0)
		return -1;
	return (int)sent;
}

// Receive exactly the way ENet does on Windows. Returns bytes, 0 for "nothing", -1 error.
static int Recv(SOCKET s, std::vector<uint8_t>* out, uint16_t* from_port)
{
	char raw[4096];
	WSABUF buf;
	buf.buf = raw;
	buf.len = sizeof(raw);
	DWORD got = 0, flags = 0;
	sockaddr_in from = {};
	int fromlen = sizeof(from);
	if (WSARecvFrom(s, &buf, 1, &got, &flags, (sockaddr*)&from, &fromlen, nullptr, nullptr) != 0)
	{
		int e = WSAGetLastError();
		return (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? 0 : -1;
	}
	out->assign(raw, raw + got);
	if (from_port)
		*from_port = ntohs(from.sin_port);
	return (int)got;
}

static bool Arrives(SOCKET s)
{
	std::vector<uint8_t> tmp;
	return Recv(s, &tmp, nullptr) > 0;
}

static std::string ExeDir()
{
	char p[MAX_PATH];
	GetModuleFileNameA(nullptr, p, MAX_PATH);
	std::string s(p);
	return s.substr(0, s.find_last_of("\\/"));
}

static std::string Ticket(int mode)
{
	return "{\"type\":\"create-ticket\",\"user\":{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"ABS#0\",\"displayName\":"
	       "\"me\"},\"search\":{\"mode\":" +
	       std::to_string(mode) + ",\"connectCode\":[]},\"appVersion\":\"3.6.4\",\"ipAddressLan\":\"127.0.0.1:41000\"}";
}

static std::string Assignment(const std::string& match_id, const std::string& code)
{
	// Pad with 16 chat messages like the real server does, to force fragmentation.
	std::string chat = "[";
	for (int i = 0; i < 16; i++)
		chat += std::string(i ? "," : "") + "\"chat message number " + std::to_string(i) + " lorem ipsum\"";
	chat += "]";
	return "{\"type\":\"get-ticket-resp\",\"matchId\":\"" + match_id +
	       "\",\"isHost\":true,\"stages\":[3,8,28,31,32,2],\"players\":["
	       "{\"isLocalPlayer\":true,\"uid\":\"me\",\"connectCode\":\"ABS#0\",\"displayName\":\"me\",\"port\":1,"
	       "\"ipAddress\":\"127.0.0.1:41000\",\"ipAddressLan\":\"127.0.0.1:41000\",\"chatMessages\":" +
	       chat +
	       "},"
	       "{\"isLocalPlayer\":false,\"uid\":\"them\",\"connectCode\":\"" +
	       code +
	       "\",\"displayName\":\"them\",\"port\":2,"
	       "\"ipAddress\":\"127.0.0.2:43114\",\"ipAddressLan\":\"\",\"chatMessages\":" +
	       chat + "}]}";
}

int main(int argc, char** argv)
{
	std::string dll = argc > 1 ? argv[1] : (ExeDir() + "\\dinput8.dll");

	// Config where the DLL looks first: <exe dir>\User\Slippi\blocklist.json
	std::string cfg_dir = ExeDir() + "\\User\\Slippi";
	CreateDirectoryA((ExeDir() + "\\User").c_str(), nullptr);
	CreateDirectoryA(cfg_dir.c_str(), nullptr);
	std::string cfg_path = cfg_dir + "\\blocklist.json";
	{
		std::ofstream f(cfg_path, std::ios::binary);
		f << "{\"enabled\":true,\"modes\":[\"unranked\"],\"beepOnSkip\":false,\"blocked\":[{\"code\":\"bad#1\",\"note\":\"test\"}]}";
	}
	DeleteFileA((cfg_dir + "\\blocklist.log").c_str());

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	HMODULE h = LoadLibraryA(dll.c_str());
	if (!h)
	{
		std::fprintf(stderr, "could not load %s (error %lu)\n", dll.c_str(), GetLastError());
		return 2;
	}
	auto version = (const char*(WINAPI*)())(void*)GetProcAddress(h, "SlippiBlocklist_Version");
	auto hooks_ok = (int(WINAPI*)())(void*)GetProcAddress(h, "SlippiBlocklist_HooksInstalled");
	auto skip_count = (unsigned long long(WINAPI*)())(void*)GetProcAddress(h, "SlippiBlocklist_SkipCount");
	auto dropped = (unsigned long long(WINAPI*)())(void*)GetProcAddress(h, "SlippiBlocklist_DroppedDatagrams");
	auto cfg_used = (const char*(WINAPI*)())(void*)GetProcAddress(h, "SlippiBlocklist_ConfigPath");
	CHECK(version && hooks_ok && skip_count && dropped && cfg_used);
	if (!(version && hooks_ok && skip_count && dropped && cfg_used))
		return 2;
	std::printf("dll version %s, hooks installed=%d, config=%s\n", version(), hooks_ok(), cfg_used());
	CHECK(hooks_ok() == 1);
	CHECK(std::string(cfg_used()) == cfg_path);

	// DirectInput forwarding really reaches the system DLL
	{
		typedef HRESULT(WINAPI * DI8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
		auto create = (DI8Create)(void*)GetProcAddress(h, "DirectInput8Create");
		CHECK(create != nullptr);
		static const GUID IID_IDirectInput8A_local = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
		IUnknown* di = nullptr;
		HRESULT hr = create ? create(GetModuleHandleA(nullptr), 0x0800, IID_IDirectInput8A_local, (LPVOID*)&di, nullptr) : E_FAIL;
		CHECK(SUCCEEDED(hr) && di != nullptr);
		if (di)
			di->Release();
	}

	SOCKET server = Bind(SERVER_IP, 43113);
	SOCKET dolphin = Bind(DOLPHIN_IP, 41000);
	SOCKET opponent = Bind(OPPONENT_IP, 43114);

	// Before any assignment, opponent traffic flows both ways
	CHECK(Send(dolphin, Reliable("hi", 1), OPPONENT_IP, 43114) > 0);
	CHECK(Arrives(opponent));
	CHECK(Send(opponent, Reliable("hi", 1), DOLPHIN_IP, 41000) > 0);
	CHECK(Arrives(dolphin));

	// Dolphin -> server: create-ticket for unranked; server -> Dolphin: accepted
	CHECK(Send(dolphin, Reliable(Ticket(1), 1), SERVER_IP, 43113) > 0);
	CHECK(Arrives(server));
	CHECK(Send(server, Reliable("{\"type\":\"create-ticket-resp\"}", 1), DOLPHIN_IP, 41000) > 0);
	CHECK(Arrives(dolphin));

	// Server assigns the blocked player, fragmented into 3 datagrams delivered out of order
	auto frags = Fragments(Assignment("mode.unranked-2026-09-12T00:00:00.000Z", "BAD#1"), 2, 3);
	CHECK(frags.size() == 3);
	CHECK(Send(server, frags[2], DOLPHIN_IP, 41000) > 0);
	CHECK(Arrives(dolphin));
	CHECK(skip_count() == 0);
	CHECK(Send(server, frags[0], DOLPHIN_IP, 41000) > 0);
	CHECK(Arrives(dolphin));
	CHECK(Send(server, frags[1], DOLPHIN_IP, 41000) > 0);
	CHECK(Arrives(dolphin));  // the datagram itself still reaches ENet untouched
	CHECK(skip_count() == 1);

	// Now everything with the opponent is dropped, in both directions
	unsigned long long before = dropped();
	CHECK(Send(dolphin, Reliable("connect", 2), OPPONENT_IP, 43114) == (int)Reliable("connect", 2).size());  // reported as sent
	CHECK(!Arrives(opponent));                                                                 // but never left
	CHECK(Send(opponent, Reliable("connect", 2), DOLPHIN_IP, 41000) > 0);
	CHECK(!Arrives(dolphin));  // swallowed on receive
	CHECK(dropped() == before + 2);

	// Server traffic and unrelated traffic are unaffected
	CHECK(Send(dolphin, Reliable(Ticket(1), 3), SERVER_IP, 43113) > 0);
	CHECK(Arrives(server));

	// Ranked assignment with a blocked player (at a different address) must not add a block
	SOCKET other = Bind(OTHER_IP, 43115);
	std::string ranked = Assignment("mode.ranked-2026", "BAD#1");
	size_t pos = ranked.find("127.0.0.2:43114");
	while (pos != std::string::npos)
	{
		ranked.replace(pos, 15, "127.0.0.3:43115");
		pos = ranked.find("127.0.0.2:43114");
	}
	CHECK(Send(dolphin, Reliable(Ticket(0), 4), SERVER_IP, 43113) > 0);
	CHECK(Arrives(server));
	for (auto& f : Fragments(ranked, 10, 2))
	{
		CHECK(Send(server, f, DOLPHIN_IP, 41000) > 0);
		CHECK(Arrives(dolphin));
	}
	CHECK(skip_count() == 1);
	CHECK(Send(dolphin, Reliable("connect", 3), OTHER_IP, 43115) > 0);
	CHECK(Arrives(other));  // not blocked: ranked is never filtered
	CHECK(Send(other, Reliable("connect", 3), DOLPHIN_IP, 41000) > 0);
	CHECK(Arrives(dolphin));
	// ...while the unranked block on the opponent is still in force
	CHECK(Send(opponent, Reliable("connect", 4), DOLPHIN_IP, 41000) > 0);
	CHECK(!Arrives(dolphin));
	closesocket(other);

	// Editing blocklist.json takes effect without reloading the DLL
	Sleep(1100);  // FILETIME granularity on some filesystems
	{
		std::ofstream f(cfg_path, std::ios::binary);
		f << "{\"enabled\":false,\"blocked\":[\"BAD#1\"]}";
	}
	CHECK(Send(dolphin, Reliable(Ticket(1), 5), SERVER_IP, 43113) > 0);
	CHECK(Arrives(server));
	CHECK(Send(server, Reliable(Assignment("mode.unranked-2026", "BAD#1"), 20), DOLPHIN_IP, 41000) > 0);
	CHECK(Arrives(dolphin));
	CHECK(skip_count() == 1);

	std::string log;
	{
		std::ifstream f(cfg_dir + "\\blocklist.log");
		std::string line;
		while (std::getline(f, line))
			log += line + "\n";
	}
	CHECK(log.find("SKIP: BAD#1 (them) [test]") != std::string::npos);
	CHECK(log.find("hooks: WSARecvFrom=ok WSASendTo=ok") != std::string::npos);
	if (g_failures)
		std::fprintf(stderr, "---- blocklist.log ----\n%s", log.c_str());

	closesocket(server);
	closesocket(dolphin);
	closesocket(opponent);
	WSACleanup();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
