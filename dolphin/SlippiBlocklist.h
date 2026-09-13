// SlippiBlocklist.h
//
// Local connect-code blocklist for Slippi matchmaking (Unranked / Teams / Party).
//
// This header is self-contained on purpose: it only depends on the C++ standard
// library and nlohmann::json (already bundled with Dolphin), so it can be dropped
// into Ishiiruka or mainline unchanged and unit-tested outside of Dolphin.
//
// File format (User/Slippi/blocklist.json, next to user.json):
//
//   {
//     "enabled": true,
//     "modes": ["unranked"],          // any of: unranked, teams, party
//     "requeueDelayMs": 1000,         // wait before asking for a new ticket
//     "blocked": [
//       "ABCD#123",                   // plain string ...
//       {"code": "WXYZ#9", "note": "why"}   // ... or object with a note
//     ]
//   }
//
// A bare JSON array of codes is also accepted.
//
// Ranked and Direct are never filtered: skipping a ranked assignment counts as a
// dodge, and in Direct you picked the code yourself.
#pragma once

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include <json.hpp>
#endif
#else
#include <json.hpp>
#endif

namespace SlippiBlocklist
{
// File name, relative to the Slippi user folder (the folder that holds user.json).
static const char* const FILE_NAME = "blocklist.json";

// Hard limits so a broken file can't stall matchmaking forever.
static const int MIN_REQUEUE_DELAY_MS = 0;
static const int MAX_REQUEUE_DELAY_MS = 30000;
static const int DEFAULT_REQUEUE_DELAY_MS = 1000;
static const int MAX_BACKOFF_MULTIPLIER = 5;

struct Entry
{
	std::string code;  // normalized, e.g. "ABCD#123"
	std::string note;  // free text, only used for logging
};

struct Config
{
	bool enabled = true;
	std::vector<std::string> modes;  // lower-case mode names that filtering applies to
	int requeue_delay_ms = DEFAULT_REQUEUE_DELAY_MS;
	bool beep_on_skip = true;  // used by the hook DLL, which has no on-screen display
	std::vector<Entry> entries;
	std::vector<std::string> warnings;  // non-fatal problems found while parsing
	bool loaded = false;                // true once a file was parsed successfully

	Config() : modes(1, "unranked") {}
};

// Trim whitespace, upper-case ASCII, and turn the full-width number sign
// (U+FF03, which is what the game itself renders) into a plain '#'.
inline std::string NormalizeCode(const std::string& raw)
{
	std::string out;
	out.reserve(raw.size());
	for (size_t i = 0; i < raw.size(); i++)
	{
		unsigned char c = static_cast<unsigned char>(raw[i]);
		if (c == 0xEF && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0xBC &&
		    static_cast<unsigned char>(raw[i + 2]) == 0x83)
		{
			out += '#';
			i += 2;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
			continue;
		out += static_cast<char>(std::toupper(c));
	}
	return out;
}

// Loose sanity check on an already-normalized code: TAG#NNN.
// Used only to warn about typos; matching itself is a plain string compare.
inline bool IsValidCode(const std::string& code)
{
	size_t hash = code.find('#');
	if (hash == std::string::npos || hash == 0 || hash > 8)
		return false;
	if (code.find('#', hash + 1) != std::string::npos)
		return false;
	for (size_t i = 0; i < hash; i++)
	{
		unsigned char c = static_cast<unsigned char>(code[i]);
		if (!std::isalnum(c))
			return false;
	}
	size_t digits = code.size() - hash - 1;
	if (digits < 1 || digits > 4)
		return false;
	for (size_t i = hash + 1; i < code.size(); i++)
	{
		unsigned char c = static_cast<unsigned char>(code[i]);
		if (!std::isdigit(c))
			return false;
	}
	return true;
}

inline std::string ToLower(std::string s)
{
	for (size_t i = 0; i < s.size(); i++)
		s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
	return s;
}

// Maps SlippiMatchmaking::OnlinePlayMode to the names used in blocklist.json.
inline std::string ModeName(int mode)
{
	switch (mode)
	{
	case 0:
		return "ranked";
	case 1:
		return "unranked";
	case 2:
		return "direct";
	case 3:
		return "teams";
	case 4:
		return "party";
	default:
		return "unknown";
	}
}

inline bool IsFilterableMode(const std::string& mode_name)
{
	return mode_name == "unranked" || mode_name == "teams" || mode_name == "party";
}

inline bool ModeAllowsFiltering(const Config& cfg, const std::string& mode_name)
{
	std::string m = ToLower(mode_name);
	if (!IsFilterableMode(m))
		return false;
	return std::find(cfg.modes.begin(), cfg.modes.end(), m) != cfg.modes.end();
}

inline const Entry* FindBlocked(const Config& cfg, const std::string& connect_code)
{
	std::string needle = NormalizeCode(connect_code);
	if (needle.empty())
		return nullptr;
	for (size_t i = 0; i < cfg.entries.size(); i++)
	{
		if (cfg.entries[i].code == needle)
			return &cfg.entries[i];
	}
	return nullptr;
}

// How long to wait before re-queuing after the Nth consecutive skip (1-based).
// Linear backoff, capped, so a server that keeps handing us the same person
// doesn't get hammered.
inline int RequeueDelayForSkip(const Config& cfg, int skip_count)
{
	int mult = skip_count < 1 ? 1 : skip_count;
	if (mult > MAX_BACKOFF_MULTIPLIER)
		mult = MAX_BACKOFF_MULTIPLIER;
	long long delay = static_cast<long long>(cfg.requeue_delay_ms) * mult;
	if (delay > MAX_REQUEUE_DELAY_MS)
		delay = MAX_REQUEUE_DELAY_MS;
	return static_cast<int>(delay);
}

inline void AddEntry(Config& cfg, const std::string& raw_code, const std::string& note)
{
	std::string code = NormalizeCode(raw_code);
	if (code.empty())
	{
		cfg.warnings.push_back("ignored empty connect code");
		return;
	}
	if (!IsValidCode(code))
		cfg.warnings.push_back("'" + code +
		                       "' does not look like a connect code (TAG#123); keeping it anyway");
	for (size_t i = 0; i < cfg.entries.size(); i++)
	{
		if (cfg.entries[i].code == code)
		{
			cfg.warnings.push_back("'" + code + "' is listed more than once");
			return;
		}
	}
	Entry e;
	e.code = code;
	e.note = note;
	cfg.entries.push_back(e);
}

// Parses blocklist.json. On a hard error (bad JSON, wrong root type) *error is
// set and a default, empty config is returned, which never blocks anyone.
inline Config Parse(const std::string& text, std::string* error)
{
	Config cfg;
	if (error)
		error->clear();

	// Editors on Windows like to prepend a UTF-8 BOM; don't let that break parsing.
	std::string body = text;
	if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
	    static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
		body.erase(0, 3);

	nlohmann::json root;
	try
	{
		root = nlohmann::json::parse(body);
	}
	catch (const std::exception& e)
	{
		if (error)
			*error = std::string("invalid JSON: ") + e.what();
		return cfg;
	}

	nlohmann::json list;
	if (root.is_array())
	{
		list = root;
	}
	else if (root.is_object())
	{
		if (root.count("enabled") && root["enabled"].is_boolean())
			cfg.enabled = root["enabled"].get<bool>();

		if (root.count("modes"))
		{
			if (!root["modes"].is_array())
			{
				cfg.warnings.push_back("'modes' must be an array; using default [\"unranked\"]");
			}
			else
			{
				cfg.modes.clear();
				for (nlohmann::json::iterator it = root["modes"].begin(); it != root["modes"].end();
				     ++it)
				{
					if (!it->is_string())
					{
						cfg.warnings.push_back("ignored non-string entry in 'modes'");
						continue;
					}
					std::string m = ToLower(it->get<std::string>());
					if (!IsFilterableMode(m))
					{
						cfg.warnings.push_back("mode '" + m +
						                       "' cannot be filtered (only unranked, teams, party); ignored");
						continue;
					}
					if (std::find(cfg.modes.begin(), cfg.modes.end(), m) == cfg.modes.end())
						cfg.modes.push_back(m);
				}
			}
		}

		if (root.count("requeueDelayMs"))
		{
			if (!root["requeueDelayMs"].is_number())
			{
				cfg.warnings.push_back("'requeueDelayMs' must be a number; using default");
			}
			else
			{
				double d = root["requeueDelayMs"].get<double>();
				if (d < MIN_REQUEUE_DELAY_MS)
					d = MIN_REQUEUE_DELAY_MS;
				if (d > MAX_REQUEUE_DELAY_MS)
					d = MAX_REQUEUE_DELAY_MS;
				cfg.requeue_delay_ms = static_cast<int>(d);
			}
		}

		if (root.count("beepOnSkip") && root["beepOnSkip"].is_boolean())
			cfg.beep_on_skip = root["beepOnSkip"].get<bool>();

		if (root.count("blocked"))
			list = root["blocked"];
		else
			cfg.warnings.push_back("no 'blocked' array found");
	}
	else
	{
		if (error)
			*error = "root must be an object or an array";
		return cfg;
	}

	if (!list.is_null() && !list.is_array())
	{
		if (error)
			*error = "'blocked' must be an array";
		return cfg;
	}

	if (list.is_array())
	{
		for (nlohmann::json::iterator it = list.begin(); it != list.end(); ++it)
		{
			if (it->is_string())
			{
				AddEntry(cfg, it->get<std::string>(), "");
			}
			else if (it->is_object())
			{
				std::string code = it->value("code", "");
				std::string note = it->value("note", "");
				AddEntry(cfg, code, note);
			}
			else
			{
				cfg.warnings.push_back("ignored entry that is neither a string nor an object");
			}
		}
	}

	cfg.loaded = true;
	return cfg;
}

// One-line description for logs.
inline std::string Summary(const Config& cfg)
{
	std::string s = cfg.enabled ? "enabled" : "disabled";
	s += ", " + std::to_string(cfg.entries.size()) + " blocked code(s), modes=";
	if (cfg.modes.empty())
		s += "(none)";
	for (size_t i = 0; i < cfg.modes.size(); i++)
		s += (i ? "," : "") + cfg.modes[i];
	s += ", requeueDelayMs=" + std::to_string(cfg.requeue_delay_ms);
	return s;
}
}  // namespace SlippiBlocklist
