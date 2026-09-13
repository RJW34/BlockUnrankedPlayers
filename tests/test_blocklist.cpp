// Unit tests for dolphin/SlippiBlocklist.h. Needs only a C++11 compiler and
// nlohmann/json.hpp on the include path. See tests/run_tests.ps1.
#include "SlippiBlocklist.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

using namespace SlippiBlocklist;

static void test_normalize()
{
	CHECK(NormalizeCode("abcd#123") == "ABCD#123");
	CHECK(NormalizeCode("  AbCd#1 \n") == "ABCD#1");
	CHECK(NormalizeCode("ABCD\xEF\xBC\x83" "123") == "ABCD#123");  // full-width number sign
	CHECK(NormalizeCode("") == "");
	CHECK(NormalizeCode("\xEF\xBC") == "\xEF\xBC");  // truncated multibyte left alone
}

static void test_valid()
{
	CHECK(IsValidCode("ABS#0"));
	CHECK(IsValidCode("ABCDEFGH#1234"));
	CHECK(IsValidCode("A1#9"));
	CHECK(!IsValidCode("ABCD"));
	CHECK(!IsValidCode("#123"));
	CHECK(!IsValidCode("ABCDEFGHI#1"));
	CHECK(!IsValidCode("ABCD#12345"));
	CHECK(!IsValidCode("AB CD#1"));
	CHECK(!IsValidCode("ABCD#1#2"));
	CHECK(!IsValidCode("ABCD#"));
}

static void test_modes()
{
	CHECK(ModeName(0) == "ranked");
	CHECK(ModeName(1) == "unranked");
	CHECK(ModeName(2) == "direct");
	CHECK(ModeName(3) == "teams");
	CHECK(ModeName(4) == "party");
	CHECK(ModeName(99) == "unknown");

	Config cfg;  // default: unranked only
	CHECK(ModeAllowsFiltering(cfg, "unranked"));
	CHECK(ModeAllowsFiltering(cfg, "UNRANKED"));
	CHECK(!ModeAllowsFiltering(cfg, "teams"));
	CHECK(!ModeAllowsFiltering(cfg, "ranked"));
	CHECK(!ModeAllowsFiltering(cfg, "direct"));

	cfg.modes.push_back("ranked");  // even if someone forces it in, ranked stays off
	CHECK(!ModeAllowsFiltering(cfg, "ranked"));
}

static void test_parse_object()
{
	std::string err;
	Config cfg = Parse(
	    "{\"enabled\": true, \"modes\": [\"unranked\", \"Teams\", \"ranked\", 5], \"requeueDelayMs\": 2500,"
	    " \"blocked\": [\"abcd#123\", {\"code\": \"wxyz#9\", \"note\": \"rude\"}, \"abcd#123\", \"\", 7]}",
	    &err);
	CHECK(err.empty());
	CHECK(cfg.loaded);
	CHECK(cfg.enabled);
	CHECK(cfg.modes.size() == 2);
	CHECK(cfg.modes[0] == "unranked");
	CHECK(cfg.modes[1] == "teams");
	CHECK(cfg.requeue_delay_ms == 2500);
	CHECK(cfg.entries.size() == 2);
	CHECK(cfg.entries[0].code == "ABCD#123");
	CHECK(cfg.entries[0].note.empty());
	CHECK(cfg.entries[1].code == "WXYZ#9");
	CHECK(cfg.entries[1].note == "rude");
	// warnings: ranked ignored, non-string mode, duplicate, empty, non-object entry
	CHECK(cfg.warnings.size() == 5);

	const Entry* hit = FindBlocked(cfg, "wxyz#9");
	CHECK(hit != nullptr && hit->note == "rude");
	CHECK(FindBlocked(cfg, "ABCD\xEF\xBC\x83" "123") != nullptr);
	CHECK(FindBlocked(cfg, "ABCD#124") == nullptr);
	CHECK(FindBlocked(cfg, "") == nullptr);
}

static void test_parse_array_and_defaults()
{
	std::string err;
	Config cfg = Parse("[\"aaaa#1\", \"bbbb#2\"]", &err);
	CHECK(err.empty());
	CHECK(cfg.loaded);
	CHECK(cfg.enabled);
	CHECK(cfg.modes.size() == 1 && cfg.modes[0] == "unranked");
	CHECK(cfg.requeue_delay_ms == DEFAULT_REQUEUE_DELAY_MS);
	CHECK(cfg.entries.size() == 2);
	CHECK(cfg.warnings.empty());

	Config empty = Parse("{}", &err);
	CHECK(err.empty());
	CHECK(empty.entries.empty());
	CHECK(empty.warnings.size() == 1);  // no 'blocked' array

	Config disabled = Parse("{\"enabled\": false, \"blocked\": [\"aaaa#1\"]}", &err);
	CHECK(!disabled.enabled);
	CHECK(disabled.entries.size() == 1);

	Config bom = Parse("\xEF\xBB\xBF[\"aaaa#1\"]", &err);
	CHECK(err.empty());
	CHECK(bom.entries.size() == 1 && bom.entries[0].code == "AAAA#1");
}

static void test_parse_errors()
{
	std::string err;
	Config bad = Parse("{not json", &err);
	CHECK(!err.empty());
	CHECK(!bad.loaded);
	CHECK(bad.entries.empty());

	Config num = Parse("42", &err);
	CHECK(err == "root must be an object or an array");

	Config badlist = Parse("{\"blocked\": \"aaaa#1\"}", &err);
	CHECK(err == "'blocked' must be an array");
	CHECK(badlist.entries.empty());

	Config clamp = Parse("{\"requeueDelayMs\": 999999, \"blocked\": []}", &err);
	CHECK(err.empty());
	CHECK(clamp.requeue_delay_ms == MAX_REQUEUE_DELAY_MS);

	Config neg = Parse("{\"requeueDelayMs\": -5, \"blocked\": []}", &err);
	CHECK(neg.requeue_delay_ms == 0);

	Config badmodes = Parse("{\"modes\": \"unranked\", \"blocked\": []}", &err);
	CHECK(err.empty());
	CHECK(badmodes.modes.size() == 1 && badmodes.modes[0] == "unranked");
}

static void test_backoff()
{
	Config cfg;
	cfg.requeue_delay_ms = 1000;
	CHECK(RequeueDelayForSkip(cfg, 0) == 1000);
	CHECK(RequeueDelayForSkip(cfg, 1) == 1000);
	CHECK(RequeueDelayForSkip(cfg, 3) == 3000);
	CHECK(RequeueDelayForSkip(cfg, 50) == 5000);
	cfg.requeue_delay_ms = 20000;
	CHECK(RequeueDelayForSkip(cfg, 5) == MAX_REQUEUE_DELAY_MS);
	cfg.requeue_delay_ms = 0;
	CHECK(RequeueDelayForSkip(cfg, 5) == 0);
}

static void test_summary()
{
	std::string err;
	Config cfg = Parse("{\"modes\": [\"teams\", \"unranked\"], \"blocked\": [\"aaaa#1\"]}", &err);
	CHECK(Summary(cfg) == "enabled, 1 blocked code(s), modes=teams,unranked, requeueDelayMs=1000");
}

int main()
{
	test_normalize();
	test_valid();
	test_modes();
	test_parse_object();
	test_parse_array_and_defaults();
	test_parse_errors();
	test_backoff();
	test_summary();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
