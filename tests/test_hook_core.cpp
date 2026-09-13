// Unit tests for hook/BlocklistHookCore.h: ENet packet walking, fragment
// reassembly, and the skip/drop decisions. No Windows dependencies.
#include "BlocklistHookCore.h"

#include <cstdio>
#include <cstdlib>
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

using namespace SlippiHook;

// ---- packet builders (big-endian, matching enet/protocol.h) ---------------- //
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
static void put32(std::vector<uint8_t>& v, uint32_t x)
{
	v.push_back(x >> 24); v.push_back((x >> 16) & 0xFF); v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}

static std::vector<uint8_t> Header(bool sent_time = true)
{
	std::vector<uint8_t> v;
	put16(v, (uint16_t)(0x0001 | (sent_time ? EnetReassembler::HEADER_FLAG_SENT_TIME : 0)));
	if (sent_time)
		put16(v, 0x1234);
	return v;
}

static void Reliable(std::vector<uint8_t>& v, uint8_t channel, uint16_t seq, const std::string& data)
{
	v.push_back(EnetReassembler::SEND_RELIABLE | 0x80);
	v.push_back(channel);
	put16(v, seq);
	put16(v, (uint16_t)data.size());
	v.insert(v.end(), data.begin(), data.end());
}

static void Ping(std::vector<uint8_t>& v, uint16_t seq)
{
	v.push_back(EnetReassembler::PING | 0x80);
	v.push_back(0xFF);
	put16(v, seq);
}

static void Ack(std::vector<uint8_t>& v, uint16_t seq)
{
	v.push_back(EnetReassembler::ACKNOWLEDGE);
	v.push_back(0xFF);
	put16(v, seq);
	put16(v, seq);
	put16(v, 0x1234);
}

// Split `data` into `count` fragments, one datagram each (unless combined).
static std::vector<std::vector<uint8_t>> Fragmented(uint8_t channel, uint16_t start_seq, const std::string& data,
                                                    uint32_t count)
{
	std::vector<std::vector<uint8_t>> out;
	uint32_t total = (uint32_t)data.size();
	uint32_t frag_len = (total + count - 1) / count;
	for (uint32_t n = 0; n < count; n++)
	{
		uint32_t off = n * frag_len;
		uint32_t len = off + frag_len > total ? total - off : frag_len;
		std::vector<uint8_t> v = Header();
		v.push_back(EnetReassembler::SEND_FRAGMENT | 0x80);
		v.push_back(channel);
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

static std::string ToString(const EnetMessage& m) { return std::string(m.data.begin(), m.data.end()); }

// ---- tests ---------------------------------------------------------------- //
static void test_helpers()
{
	uint32_t ip = 0;
	uint16_t port = 0;
	CHECK(ParseIpPort("203.0.113.7:45000", &ip, &port));
	CHECK(port == 45000);
	CHECK(IpToString(ip) == "203.0.113.7");
	uint8_t bytes[4];
	std::memcpy(bytes, &ip, 4);
	CHECK(bytes[0] == 203 && bytes[1] == 0 && bytes[2] == 113 && bytes[3] == 7);  // network byte order
	CHECK(ParseIpPort("10.0.0.1", &ip, &port) && port == 0);
	CHECK(!ParseIpPort("", &ip, &port));
	CHECK(!ParseIpPort("nope:1", &ip, &port));
	CHECK(!ParseIpPort("300.1.1.1:5", &ip, &port));
	CHECK(!ParseIpPort("1.2.3:5", &ip, &port));
}

static void test_reassembler_simple()
{
	EnetReassembler r;
	std::vector<uint8_t> pkt = Header();
	Ack(pkt, 1);
	Ping(pkt, 2);
	Reliable(pkt, 0, 3, "{\"a\":1}");
	Reliable(pkt, 1, 4, "second");
	auto msgs = r.Feed(pkt.data(), pkt.size(), 1000);
	CHECK(msgs.size() == 2);
	CHECK(msgs.size() == 2 && ToString(msgs[0]) == "{\"a\":1}" && msgs[0].channel == 0);
	CHECK(msgs.size() == 2 && ToString(msgs[1]) == "second" && msgs[1].channel == 1);

	// Header without sent time
	std::vector<uint8_t> pkt2 = Header(false);
	Reliable(pkt2, 0, 5, "x");
	msgs = r.Feed(pkt2.data(), pkt2.size(), 1000);
	CHECK(msgs.size() == 1 && ToString(msgs[0]) == "x");

	// Compressed flag -> nothing parsed, flag recorded
	std::vector<uint8_t> pkt3;
	put16(pkt3, 0x0001 | EnetReassembler::HEADER_FLAG_COMPRESSED);
	Reliable(pkt3, 0, 6, "zzz");
	msgs = r.Feed(pkt3.data(), pkt3.size(), 1000);
	CHECK(msgs.empty());
	CHECK(r.compressed_seen);

	// Truncated data length must not read past the buffer
	std::vector<uint8_t> pkt4 = Header();
	Reliable(pkt4, 0, 7, "hello world");
	pkt4.resize(pkt4.size() - 4);
	msgs = r.Feed(pkt4.data(), pkt4.size(), 1000);
	CHECK(msgs.empty());

	// Garbage
	uint8_t junk[3] = {0xFF, 0xFF, 0xFF};
	msgs = r.Feed(junk, 3, 1000);
	CHECK(msgs.empty());
	msgs = r.Feed(junk, 0, 1000);
	CHECK(msgs.empty());
}

static void test_reassembler_fragments()
{
	std::string big;
	for (int i = 0; i < 3000; i++)
		big += (char)('a' + (i % 26));

	EnetReassembler r;
	auto frags = Fragmented(0, 10, big, 3);
	CHECK(frags.size() == 3);
	auto m = r.Feed(frags[0].data(), frags[0].size(), 1000);
	CHECK(m.empty() && r.PendingFragments() == 1);
	// duplicate (retransmit) of fragment 0 is harmless
	m = r.Feed(frags[0].data(), frags[0].size(), 1001);
	CHECK(m.empty() && r.PendingFragments() == 1);
	// out of order
	m = r.Feed(frags[2].data(), frags[2].size(), 1002);
	CHECK(m.empty());
	m = r.Feed(frags[1].data(), frags[1].size(), 1003);
	CHECK(m.size() == 1);
	CHECK(m.size() == 1 && ToString(m[0]) == big);
	CHECK(r.PendingFragments() == 0);

	// Fragments interleaved with other commands in the same datagram
	auto frags2 = Fragmented(0, 20, big, 2);
	std::vector<uint8_t> combo = Header();
	Ping(combo, 99);
	combo.insert(combo.end(), frags2[0].begin() + 4, frags2[0].end());  // strip header of frag
	Reliable(combo, 0, 21, "tail");
	m = r.Feed(combo.data(), combo.size(), 2000);
	CHECK(m.size() == 1 && ToString(m[0]) == "tail");
	m = r.Feed(frags2[1].data(), frags2[1].size(), 2001);
	CHECK(m.size() == 1 && ToString(m[0]) == big);

	// Stale fragment expires
	auto frags3 = Fragmented(0, 30, big, 2);
	m = r.Feed(frags3[0].data(), frags3[0].size(), 3000);
	CHECK(r.PendingFragments() == 1);
	std::vector<uint8_t> ping = Header();
	Ping(ping, 1);
	r.Feed(ping.data(), ping.size(), 3000 + FRAGMENT_TTL_MS + 1);
	CHECK(r.PendingFragments() == 0);

	// A connect resets pending fragments
	m = r.Feed(frags3[0].data(), frags3[0].size(), 4000);
	CHECK(r.PendingFragments() == 1);
	std::vector<uint8_t> conn = Header();
	conn.push_back(EnetReassembler::VERIFY_CONNECT | 0x80);
	conn.push_back(0xFF);
	put16(conn, 1);
	conn.resize(conn.size() + 44 - 4, 0);
	bool saw = false;
	r.Feed(conn.data(), conn.size(), 4001, &saw);
	CHECK(saw);
	CHECK(r.PendingFragments() == 0);

	// Bogus fragment metadata is ignored, not crashed on
	std::vector<uint8_t> bad = Header();
	bad.push_back(EnetReassembler::SEND_FRAGMENT);
	bad.push_back(0);
	put16(bad, 40);
	put16(bad, 40);
	put16(bad, 4);
	put32(bad, 2);
	put32(bad, 5);  // fragmentNumber >= fragmentCount
	put32(bad, 100);
	put32(bad, 0);
	bad.insert(bad.end(), {'a', 'b', 'c', 'd'});
	m = r.Feed(bad.data(), bad.size(), 5000);
	CHECK(m.empty() && r.PendingFragments() == 0);
}

static SlippiBlocklist::Config MakeConfig(const std::string& json)
{
	std::string err;
	auto cfg = SlippiBlocklist::Parse(json, &err);
	CHECK(err.empty());
	return cfg;
}

static std::string TicketResp(const std::string& match_id, const std::string& remote_code,
                              const std::string& remote_ip = "203.0.113.7:45000",
                              const std::string& remote_lan = "192.168.1.9:45000", bool remote_first = false)
{
	std::string local = "{\"isLocalPlayer\":true,\"connectCode\":\"ABS#0\",\"displayName\":\"me\",\"port\":" +
	                    std::string(remote_first ? "2" : "1") + ",\"ipAddress\":\"198.51.100.5:41000\"}";
	std::string remote = "{\"isLocalPlayer\":false,\"connectCode\":\"" + remote_code +
	                     "\",\"displayName\":\"them\",\"port\":" + std::string(remote_first ? "1" : "2") +
	                     ",\"ipAddress\":\"" + remote_ip + "\",\"ipAddressLan\":\"" + remote_lan + "\"}";
	return "{\"type\":\"get-ticket-resp\",\"matchId\":\"" + match_id + "\",\"isHost\":true,\"stages\":[3,8],\"players\":[" +
	       (remote_first ? remote + "," + local : local + "," + remote) + "]}";
}

static std::vector<uint8_t> Datagram(const std::string& json, uint16_t seq = 1)
{
	std::vector<uint8_t> v = Header();
	Reliable(v, 0, seq, json);
	return v;
}

static void test_hook_skip_flow()
{
	BlocklistHook h;
	std::vector<std::string> log;
	int skips = 0;
	SkipEvent last;
	h.log = [&](const std::string& s) { log.push_back(s); };
	h.on_skip = [&](const SkipEvent& e) { skips++; last = e; };
	h.SetConfig(MakeConfig("{\"blocked\":[{\"code\":\"bad#1\",\"note\":\"rude\"}]}"));

	uint32_t remote_ip = 0, remote_lan = 0, other_ip = 0;
	ParseIpPort("203.0.113.7", &remote_ip, nullptr);
	ParseIpPort("192.168.1.9", &remote_lan, nullptr);
	ParseIpPort("203.0.113.99", &other_ip, nullptr);

	// Dolphin sends create-ticket for unranked
	std::string ticket = "{\"type\":\"create-ticket\",\"user\":{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"ABS#0\","
	                     "\"displayName\":\"me\"},\"search\":{\"mode\":1,\"connectCode\":[]},\"appVersion\":\"3.6.4\"}";
	auto d = Datagram(ticket);
	h.OnMmOutgoing(d.data(), d.size(), 1000);
	CHECK(h.LastRequestedMode() == 1);
	CHECK(h.LocalCode() == "ABS#0");

	d = Datagram("{\"type\":\"create-ticket-resp\"}");
	h.OnMmIncoming(d.data(), d.size(), 1100);

	// Server assigns a blocked opponent, message fragmented across 2 datagrams
	std::string resp = TicketResp("mode.unranked-2026-09-12T01:02:03.000Z", "BAD#1");
	auto frags = Fragmented(0, 2, resp, 2);
	h.OnMmIncoming(frags[0].data(), frags[0].size(), 5000);
	CHECK(skips == 0);
	h.OnMmIncoming(frags[1].data(), frags[1].size(), 5001);
	CHECK(skips == 1);
	CHECK(h.SkipCount() == 1);
	CHECK(last.code == "BAD#1" && last.display_name == "them" && last.note == "rude" && last.mode == "unranked");
	CHECK(last.ips.size() == 2);
	CHECK(h.BlockedAddressCount(5001) == 2);

	// Both of their addresses are dropped, an unrelated one is not, for the block window
	CHECK(h.ShouldDrop(remote_ip, 5002));
	CHECK(h.ShouldDrop(remote_lan, 5002));
	CHECK(!h.ShouldDrop(other_ip, 5002));
	CHECK(h.ShouldDrop(remote_ip, 5001 + BLOCK_WINDOW_MS - 1));
	CHECK(!h.ShouldDrop(remote_ip, 5001 + BLOCK_WINDOW_MS + 1));
	CHECK(h.BlockedAddressCount(5001 + BLOCK_WINDOW_MS + 1) == 0);
	CHECK(h.DroppedDatagrams() == 3);

	bool logged_skip = false;
	for (const auto& s : log)
		logged_skip = logged_skip || s.find("SKIP: BAD#1") != std::string::npos;
	CHECK(logged_skip);

	// Same person, but now Dolphin is queued for ranked: never filtered
	auto t2 = Datagram(ticket);
	std::string ranked = ticket;
	ranked.replace(ranked.find("\"mode\":1"), 8, "\"mode\":0");
	t2 = Datagram(ranked);
	h.OnMmOutgoing(t2.data(), t2.size(), 100000);
	CHECK(h.LastRequestedMode() == 0);
	d = Datagram(TicketResp("mode.ranked-x", "BAD#1"));
	h.OnMmIncoming(d.data(), d.size(), 100001);
	CHECK(skips == 1);
	CHECK(h.BlockedAddressCount(100002) == 0);

	// Direct: never filtered either, even if the file says so
	h.SetConfig(MakeConfig("{\"modes\":[\"unranked\",\"direct\",\"teams\"],\"blocked\":[\"BAD#1\"]}"));
	std::string direct = ticket;
	direct.replace(direct.find("\"mode\":1"), 8, "\"mode\":2");
	d = Datagram(direct);
	h.OnMmOutgoing(d.data(), d.size(), 200000);
	d = Datagram(TicketResp("mode.direct-x", "BAD#1"));
	h.OnMmIncoming(d.data(), d.size(), 200001);
	CHECK(skips == 1);

	// Teams is filtered when enabled
	std::string teams = ticket;
	teams.replace(teams.find("\"mode\":1"), 8, "\"mode\":3");
	d = Datagram(teams);
	h.OnMmOutgoing(d.data(), d.size(), 300000);
	d = Datagram(TicketResp("mode.teams-x", "bad#1"));
	h.OnMmIncoming(d.data(), d.size(), 300001);
	CHECK(skips == 2);

	// Unranked with a non-blocked opponent: nothing happens
	d = Datagram(ticket);
	h.OnMmOutgoing(d.data(), d.size(), 400000);
	d = Datagram(TicketResp("mode.unranked-x", "GOOD#2"));
	h.OnMmIncoming(d.data(), d.size(), 400001);
	CHECK(skips == 2);
	CHECK(h.BlockedAddressCount(400002) == 0);

	// Never skip ourselves even if our own code is somehow on the list
	h.SetConfig(MakeConfig("{\"blocked\":[\"ABS#0\"]}"));
	d = Datagram(ticket);
	h.OnMmOutgoing(d.data(), d.size(), 500000);
	d = Datagram(TicketResp("mode.unranked-x", "GOOD#2", "203.0.113.7:45000", "", true));
	h.OnMmIncoming(d.data(), d.size(), 500001);
	CHECK(skips == 2);

	// Disabled config: nothing
	h.SetConfig(MakeConfig("{\"enabled\":false,\"blocked\":[\"BAD#1\"]}"));
	d = Datagram(TicketResp("mode.unranked-x", "BAD#1"));
	h.OnMmIncoming(d.data(), d.size(), 600001);
	CHECK(skips == 2);

	// Mode falls back to the matchId when no create-ticket was seen (e.g. DLL loaded late)
	BlocklistHook h2;
	int skips2 = 0;
	h2.on_skip = [&](const SkipEvent&) { skips2++; };
	h2.SetConfig(MakeConfig("{\"blocked\":[\"BAD#1\"]}"));
	d = Datagram(TicketResp("mode.unranked-2026", "BAD#1"));
	h2.OnMmIncoming(d.data(), d.size(), 10);
	CHECK(skips2 == 1);
	d = Datagram(TicketResp("mode.ranked-2026", "BAD#1"));
	h2.OnMmIncoming(d.data(), d.size(), 20);
	CHECK(skips2 == 1);

	// Full-width number sign from the server side still matches
	d = Datagram(TicketResp("mode.unranked-2026", "BAD\xEF\xBC\x83" "1"));
	h2.OnMmIncoming(d.data(), d.size(), 30);
	CHECK(skips2 == 2);

	// Non-JSON and error responses are ignored quietly
	d = Datagram("not json at all");
	h2.OnMmIncoming(d.data(), d.size(), 40);
	d = Datagram("{\"type\":\"get-ticket-resp\",\"error\":\"outdated\"}");
	h2.OnMmIncoming(d.data(), d.size(), 41);
	d = Datagram("{\"type\":\"get-ticket-resp\",\"players\":\"nope\"}");
	h2.OnMmIncoming(d.data(), d.size(), 42);
	CHECK(skips2 == 2);
}

int main()
{
	test_helpers();
	test_reassembler_simple();
	test_reassembler_fragments();
	test_hook_skip_flow();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
