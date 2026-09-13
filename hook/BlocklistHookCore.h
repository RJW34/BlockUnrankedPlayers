// BlocklistHookCore.h
//
// Platform-independent brains of the drop-in blocklist DLL.
//
// Slippi Dolphin talks to the matchmaking server (mm.slippi.gg:43113) over ENet.
// The server's "get-ticket-resp" message lists every player in the match with
// their connect code and public IP:port. Dolphin then opens a direct ENet
// connection to each remote player; if that connection does not come up within
// 8 seconds Dolphin gives up and automatically asks the server for a new match
// ("Connection attempt failed, looking for someone else").
//
// This code watches the datagrams that flow to and from the matchmaking server
// (read-only: nothing is ever modified), reassembles the JSON messages out of
// the ENet packets, and when a blocked connect code shows up in an assignment
// it remembers that player's addresses for a short window. The DLL then drops
// every datagram to or from those addresses, so Dolphin's own timeout-and-retry
// path takes care of finding someone else.
//
// No Windows headers here so it can be unit-tested with any C++17 compiler.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "SlippiBlocklist.h"

namespace SlippiHook
{
static const uint16_t MM_PORT = 43113;
// How long to keep dropping traffic to a skipped player's addresses. Dolphin's
// connect timeout is 8 s; the window covers that plus the opponent's retries.
static const uint64_t BLOCK_WINDOW_MS = 30000;
// Give up on a half-received fragmented message after this long.
static const uint64_t FRAGMENT_TTL_MS = 15000;
static const uint32_t MAX_MESSAGE_BYTES = 1u << 20;

// ------------------------------------------------------------------------- //
// Small helpers
// ------------------------------------------------------------------------- //
inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// "203.0.113.7:45000" -> ip (network byte order, i.e. the same u32 sockaddr_in
// holds), port. Returns false for anything that is not a dotted IPv4 address.
inline bool ParseIpPort(const std::string& s, uint32_t* ip_be, uint16_t* port)
{
	size_t colon = s.find(':');
	std::string host = colon == std::string::npos ? s : s.substr(0, colon);
	unsigned a = 0, b = 0, c = 0, d = 0;
	char extra = 0;
	if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
		return false;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return false;
	uint8_t bytes[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
	std::memcpy(ip_be, bytes, 4);
	unsigned p = 0;
	if (colon != std::string::npos)
		std::sscanf(s.c_str() + colon + 1, "%u", &p);
	if (port)
		*port = (uint16_t)(p & 0xFFFF);
	return true;
}

inline std::string IpToString(uint32_t ip_be)
{
	uint8_t b[4];
	std::memcpy(b, &ip_be, 4);
	return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
	       std::to_string(b[3]);
}

// ------------------------------------------------------------------------- //
// ENet packet walker. Pulls complete messages (reliable, unreliable, and
// reassembled fragments) out of raw datagrams. Mirrors enet/protocol.h.
// ------------------------------------------------------------------------- //
struct EnetMessage
{
	uint8_t channel;
	std::vector<uint8_t> data;
};

class EnetReassembler
{
public:
	enum Command
	{
		NONE = 0,
		ACKNOWLEDGE = 1,
		CONNECT = 2,
		VERIFY_CONNECT = 3,
		DISCONNECT = 4,
		PING = 5,
		SEND_RELIABLE = 6,
		SEND_UNRELIABLE = 7,
		SEND_FRAGMENT = 8,
		SEND_UNSEQUENCED = 9,
		BANDWIDTH_LIMIT = 10,
		THROTTLE_CONFIGURE = 11,
		SEND_UNRELIABLE_FRAGMENT = 12,
		COUNT = 13,
	};
	static const uint16_t HEADER_FLAG_COMPRESSED = 1 << 14;
	static const uint16_t HEADER_FLAG_SENT_TIME = 1 << 15;

	// sizeof() of each fixed command struct in enet/protocol.h (packed).
	static size_t CommandSize(int cmd)
	{
		static const size_t sizes[COUNT] = {0, 8, 48, 44, 8, 4, 6, 8, 24, 8, 12, 16, 24};
		return (cmd > 0 && cmd < COUNT) ? sizes[cmd] : 0;
	}

	// Feed one datagram. Returns every message completed by it. `saw_connect`
	// is set when the datagram carried a CONNECT/VERIFY_CONNECT, which means a
	// fresh session whose sequence numbers restart.
	std::vector<EnetMessage> Feed(const uint8_t* buf, size_t len, uint64_t now_ms, bool* saw_connect = nullptr)
	{
		std::vector<EnetMessage> out;
		if (saw_connect)
			*saw_connect = false;
		if (len < 2)
			return out;
		uint16_t peer_id = be16(buf);
		if (peer_id & HEADER_FLAG_COMPRESSED)
		{
			compressed_seen = true;  // Slippi never negotiates compression; nothing we can do
			return out;
		}
		size_t pos = 2 + ((peer_id & HEADER_FLAG_SENT_TIME) ? 2 : 0);

		ExpireFragments(now_ms);

		while (pos + 4 <= len)
		{
			int cmd = buf[pos] & 0x0F;
			uint8_t channel = buf[pos + 1];
			size_t fixed = CommandSize(cmd);
			if (fixed == 0 || pos + fixed > len)
				break;
			const uint8_t* p = buf + pos;

			switch (cmd)
			{
			case CONNECT:
			case VERIFY_CONNECT:
				if (saw_connect)
					*saw_connect = true;
				fragments.clear();
				pos += fixed;
				break;
			case SEND_RELIABLE:
			{
				uint16_t data_len = be16(p + 4);
				if (pos + fixed + data_len > len)
					return out;
				EnetMessage m;
				m.channel = channel;
				m.data.assign(p + fixed, p + fixed + data_len);
				out.push_back(m);
				pos += fixed + data_len;
				break;
			}
			case SEND_UNRELIABLE:
			case SEND_UNSEQUENCED:
			{
				uint16_t data_len = be16(p + 6);
				if (pos + fixed + data_len > len)
					return out;
				EnetMessage m;
				m.channel = channel;
				m.data.assign(p + fixed, p + fixed + data_len);
				out.push_back(m);
				pos += fixed + data_len;
				break;
			}
			case SEND_FRAGMENT:
			case SEND_UNRELIABLE_FRAGMENT:
			{
				uint16_t start_seq = be16(p + 4);
				uint16_t data_len = be16(p + 6);
				uint32_t frag_count = be32(p + 8);
				uint32_t frag_num = be32(p + 12);
				uint32_t total_len = be32(p + 16);
				uint32_t frag_off = be32(p + 20);
				if (pos + fixed + data_len > len)
					return out;
				const uint8_t* data = p + fixed;
				pos += fixed + data_len;

				if (frag_count == 0 || frag_count > 65536 || frag_num >= frag_count || total_len > MAX_MESSAGE_BYTES ||
				    (uint64_t)frag_off + data_len > total_len)
					break;

				uint32_t key = ((uint32_t)channel << 16) | start_seq;
				Fragment& f = fragments[key];
				if (f.data.empty())
				{
					f.channel = channel;
					f.total = total_len;
					f.count = frag_count;
					f.data.assign(total_len, 0);
					f.got.assign(frag_count, false);
					f.received = 0;
					f.started_ms = now_ms;
				}
				else if (f.total != total_len || f.count != frag_count)
				{
					// Stale entry from a previous session with the same sequence number.
					f = Fragment();
					f.channel = channel;
					f.total = total_len;
					f.count = frag_count;
					f.data.assign(total_len, 0);
					f.got.assign(frag_count, false);
					f.received = 0;
					f.started_ms = now_ms;
				}
				if (!f.got[frag_num])
				{
					std::memcpy(&f.data[frag_off], data, data_len);
					f.got[frag_num] = true;
					f.received++;
				}
				if (f.received == f.count)
				{
					EnetMessage m;
					m.channel = channel;
					m.data.swap(f.data);
					out.push_back(m);
					fragments.erase(key);
				}
				break;
			}
			default:
				pos += fixed;
				break;
			}
		}
		return out;
	}

	void Reset() { fragments.clear(); }
	size_t PendingFragments() const { return fragments.size(); }
	bool compressed_seen = false;

private:
	struct Fragment
	{
		uint8_t channel = 0;
		uint32_t total = 0;
		uint32_t count = 0;
		uint32_t received = 0;
		uint64_t started_ms = 0;
		std::vector<uint8_t> data;
		std::vector<bool> got;
	};
	std::map<uint32_t, Fragment> fragments;

	void ExpireFragments(uint64_t now_ms)
	{
		for (auto it = fragments.begin(); it != fragments.end();)
		{
			if (now_ms - it->second.started_ms > FRAGMENT_TTL_MS)
				it = fragments.erase(it);
			else
				++it;
		}
	}
};

// ------------------------------------------------------------------------- //
// The decision maker
// ------------------------------------------------------------------------- //
struct SkipEvent
{
	std::string mode;
	std::string code;
	std::string display_name;
	std::string note;
	std::vector<uint32_t> ips;  // network byte order
	bool beep = true;           // copy of Config::beep_on_skip at the time of the skip
};

class BlocklistHook
{
public:
	std::function<void(const std::string&)> log;   // optional
	std::function<void(const SkipEvent&)> on_skip;  // optional (beep, etc.)

	void SetConfig(const SlippiBlocklist::Config& cfg)
	{
		std::lock_guard<std::mutex> lock(mu_);
		cfg_ = cfg;
	}
	SlippiBlocklist::Config GetConfig()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return cfg_;
	}

	// Datagram Dolphin sent to the matchmaking server.
	void OnMmOutgoing(const uint8_t* buf, size_t len, uint64_t now_ms)
	{
		std::vector<std::string> logs;
		{
			std::lock_guard<std::mutex> lock(mu_);
			bool connect = false;
			auto msgs = out_.Feed(buf, len, now_ms, &connect);
			if (connect)
				in_.Reset();
			for (const auto& m : msgs)
				HandleOutgoingMessage(m, &logs);
		}
		Flush(logs);
	}

	// Datagram received from the matchmaking server.
	void OnMmIncoming(const uint8_t* buf, size_t len, uint64_t now_ms)
	{
		std::vector<std::string> logs;
		std::vector<SkipEvent> skips;
		{
			std::lock_guard<std::mutex> lock(mu_);
			auto msgs = in_.Feed(buf, len, now_ms);
			for (const auto& m : msgs)
				HandleIncomingMessage(m, now_ms, &logs, &skips);
		}
		// Callbacks run with the lock released so they may call back into us.
		Flush(logs);
		if (on_skip)
			for (const auto& ev : skips)
				on_skip(ev);
	}

	// Should a datagram to/from this IPv4 address (network byte order) be dropped?
	bool ShouldDrop(uint32_t ip_be, uint64_t now_ms)
	{
		std::lock_guard<std::mutex> lock(mu_);
		bool drop = false;
		for (auto it = blocked_.begin(); it != blocked_.end();)
		{
			if (now_ms >= it->until_ms)
			{
				it = blocked_.erase(it);
				continue;
			}
			if (it->ip_be == ip_be)
				drop = true;
			++it;
		}
		if (drop)
			dropped_datagrams_++;
		return drop;
	}

	// Introspection for tests / logs
	int LastRequestedMode()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return last_mode_;
	}
	std::string LocalCode()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return local_code_;
	}
	size_t BlockedAddressCount(uint64_t now_ms)
	{
		std::lock_guard<std::mutex> lock(mu_);
		size_t n = 0;
		for (const auto& b : blocked_)
			if (now_ms < b.until_ms)
				n++;
		return n;
	}
	uint64_t DroppedDatagrams()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return dropped_datagrams_;
	}
	uint64_t SkipCount()
	{
		std::lock_guard<std::mutex> lock(mu_);
		return skips_;
	}

private:
	struct Blocked
	{
		uint32_t ip_be;
		uint64_t until_ms;
	};

	std::mutex mu_;
	SlippiBlocklist::Config cfg_;
	EnetReassembler in_, out_;
	std::vector<Blocked> blocked_;
	int last_mode_ = -1;
	std::string local_code_;
	uint64_t dropped_datagrams_ = 0;
	uint64_t skips_ = 0;

	void Flush(const std::vector<std::string>& logs)
	{
		if (!log)
			return;
		for (const auto& s : logs)
			log(s);
	}

	static bool ParseJson(const std::vector<uint8_t>& data, nlohmann::json* out)
	{
		if (data.empty() || data[0] != '{')
			return false;
		try
		{
			*out = nlohmann::json::parse(data.begin(), data.end());
		}
		catch (...)
		{
			return false;
		}
		return out->is_object();
	}

	void HandleOutgoingMessage(const EnetMessage& m, std::vector<std::string>* logs)
	{
		nlohmann::json j;
		if (!ParseJson(m.data, &j))
			return;
		if (j.value("type", "") != "create-ticket")
			return;
		last_mode_ = -1;
		if (j.count("search") && j["search"].is_object() && j["search"].count("mode") && j["search"]["mode"].is_number())
			last_mode_ = j["search"]["mode"].get<int>();
		if (j.count("user") && j["user"].is_object())
			local_code_ = SlippiBlocklist::NormalizeCode(j["user"].value("connectCode", ""));
		logs->push_back("create-ticket sent (mode=" + SlippiBlocklist::ModeName(last_mode_) + ", me=" + local_code_ +
		                ", blocklist " + SlippiBlocklist::Summary(cfg_) + ")");
	}

	static std::string ModeFromMatchId(const std::string& match_id)
	{
		static const char* names[] = {"ranked", "unranked", "direct", "teams", "party"};
		for (const char* n : names)
			if (match_id.find(std::string("mode.") + n) != std::string::npos)
				return n;
		return "unknown";
	}

	void HandleIncomingMessage(const EnetMessage& m, uint64_t now_ms, std::vector<std::string>* logs,
	                           std::vector<SkipEvent>* skips)
	{
		nlohmann::json j;
		if (!ParseJson(m.data, &j))
			return;
		std::string type = j.value("type", "");
		if (type == "create-ticket-resp")
		{
			std::string err = j.value("error", "");
			logs->push_back(err.empty() ? "ticket accepted by server" : "ticket rejected: " + err);
			return;
		}
		if (type != "get-ticket-resp")
			return;
		if (!j.value("error", "").empty())
		{
			logs->push_back("server error: " + j.value("error", ""));
			return;
		}

		std::string match_id = j.value("matchId", "");
		std::string mode = last_mode_ >= 0 ? SlippiBlocklist::ModeName(last_mode_) : ModeFromMatchId(match_id);
		if (mode == "unknown")
			mode = ModeFromMatchId(match_id);

		if (!j.count("players") || !j["players"].is_array())
			return;

		if (!cfg_.enabled || cfg_.entries.empty())
		{
			logs->push_back("match found (" + mode + "); blocklist " + (cfg_.enabled ? "empty" : "disabled") +
			                ", not filtering");
			return;
		}
		if (!SlippiBlocklist::ModeAllowsFiltering(cfg_, mode))
		{
			logs->push_back("match found (" + mode + "); this mode is not filtered");
			return;
		}

		std::string players_desc;
		for (const auto& el : j["players"])
		{
			if (!el.is_object())
				continue;
			bool is_local = el.value("isLocalPlayer", false);
			std::string code = SlippiBlocklist::NormalizeCode(el.value("connectCode", ""));
			std::string name = el.value("displayName", "");
			if (!players_desc.empty())
				players_desc += ", ";
			players_desc += code + (is_local ? " (me)" : "");
			if (is_local || (!local_code_.empty() && code == local_code_))
				continue;

			const SlippiBlocklist::Entry* hit = SlippiBlocklist::FindBlocked(cfg_, code);
			if (!hit)
				continue;

			SkipEvent ev;
			ev.mode = mode;
			ev.code = code;
			ev.display_name = name;
			ev.note = hit->note;
			ev.beep = cfg_.beep_on_skip;
			const char* keys[] = {"ipAddress", "ipAddressLan"};
			for (const char* k : keys)
			{
				uint32_t ip = 0;
				uint16_t port = 0;
				std::string s = el.value(k, "");
				if (!s.empty() && ParseIpPort(s, &ip, &port))
				{
					bool dup = false;
					for (uint32_t x : ev.ips)
						dup = dup || x == ip;
					if (!dup)
						ev.ips.push_back(ip);
				}
			}
			for (uint32_t ip : ev.ips)
				blocked_.push_back(Blocked{ip, now_ms + BLOCK_WINDOW_MS});
			skips_++;

			std::string ips;
			for (uint32_t ip : ev.ips)
				ips += (ips.empty() ? "" : ", ") + IpToString(ip);
			logs->push_back("SKIP: " + code + " (" + name + ")" + (hit->note.empty() ? "" : " [" + hit->note + "]") +
			                " is blocked; dropping traffic with " + (ips.empty() ? "(no address!)" : ips) + " for " +
			                std::to_string(BLOCK_WINDOW_MS / 1000) + "s so Dolphin searches again. match=" + match_id);
			skips->push_back(ev);
		}
		logs->push_back("match found (" + mode + "): " + players_desc);
	}
};
}  // namespace SlippiHook
