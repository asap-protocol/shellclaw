/**
 * @file test_context.c
 * @brief get_context tests with SHELLCLAW_CONTEXT_TEST stubs (no network).
 */
#define _POSIX_C_SOURCE 200809L

#include "tools/context.h"
#include "core/config.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef SHELLCLAW_CONTEXT_TEST
#error "BUILD must define SHELLCLAW_CONTEXT_TEST when compiling test_context."
#endif

#define CHECK(x, msg)                                                                                \
	do {                                                                                         \
		if (!(x)) {                                                                          \
			fprintf(stderr, "%s\n", msg);                                                \
			return 1;                                                                    \
		}                                                                                    \
	} while (0)

static const char *GEO_OK = "{\"status\":\"success\",\"city\":\"Berlin\",\"country\":\"Germany\","
			    "\"countryCode\":\"DE\",\"timezone\":\"Europe/Berlin\","
			    "\"lat\":52.5,\"lon\":13.405}";

static const char *GEO_FAIL = "{\"status\":\"fail\"}";

static const char *GEO_MISSING_LAT =
	"{\"status\":\"success\",\"city\":\"Berlin\",\"country\":\"Germany\","
	"\"countryCode\":\"DE\",\"timezone\":\"Europe/Berlin\",\"lon\":13.405}";

static const char *WX_A = "{\"daily\":{\"time\":[\"2027-03-09\"],"
		       "\"temperature_2m_max\":[99.6],\"temperature_2m_min\":[11.3],"
		       "\"precipitation_sum\":[0.5],\"weathercode\":[1]}}";

static const char *WX_B = "{\"daily\":{\"time\":[\"2027-03-09\"],"
		       "\"temperature_2m_max\":[12.05],\"temperature_2m_min\":[11.3],"
		       "\"precipitation_sum\":[0.5],\"weathercode\":[1]}}";

static const char *HY_OK = "[{\"date\":\"2027-03-10\",\"localName\":\"DayX\"}]";

static const char *HY_NEW_YEAR = "[{\"date\":\"2027-12-31\",\"localName\":\"Silvester\"},"
				 "{\"date\":\"2028-01-01\",\"localName\":\"Neujahr\"}]";
static const char *HY_BOTH =
	"[{\"date\":\"2027-03-09\",\"localName\":\"TodayH\"},{\"date\":\"2027-03-10\",\"localName\":\"DayX\"}]";

static const char *WX_FAIL = "{\"status\":\"fail\"}";

/** Sentinel for SHELLCLAW_CONTEXT_TEST fake HTTP (see context_http.c). */
static const char *HTTP_FAIL = "__CTX_HTTP_FAIL__";

static const char *GEO_BAD = "{not json";


static time_t t_march9_2027(void)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 127;
	tm.tm_mon = 2;
	tm.tm_mday = 9;
	tm.tm_hour = 10;
	return mktime(&tm);
}

static time_t t_dec31_2027(void)
{
	struct tm tm;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = 127;
	tm.tm_mon = 11;
	tm.tm_mday = 31;
	tm.tm_hour = 12;
	return mktime(&tm);
}

static int write_agent_fallback_toml(const char *path)
{
	FILE *fp;

	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fprintf(fp,
	        "[channels.discord]\n"
	        "enabled = false\n"
	        "[agent]\n"
	        "model = \"stub\"\n"
	        "timezone = \"Europe/Berlin\"\n"
	        "latitude = 48.8566\n"
	        "longitude = 2.3522\n"
	        "country_code = \"FR\"\n"
	        "[providers]\n"
	        "fallback_chain = [\"stub\"]\n");
	fclose(fp);
	return 0;
}

int main(void)
{
	char out1[8192];
	char out2[8192];
	char out3[8192];
	char out_fallback[8192];
	const tool_t *tool;
	time_t wall;
	config_t *cfg = NULL;
	char errbuf[256];
	char tmpl[] = "/tmp/shellclaw_test_context_XXXXXX";
	int fd;

	curl_global_init(CURL_GLOBAL_DEFAULT);

	tool_context_test_reset();
	CHECK(strncmp(tool_context_test_geo_url(), "https://", 8) == 0,
	      "geo API URL must use HTTPS");

	wall = t_march9_2027();
	tool_context_test_set_unix_time(wall);
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, WX_A, HY_OK);
	tool = tool_context_get();
	CHECK(tool != NULL, "missing tool");
	CHECK(tool->execute("{}", out1, sizeof(out1)) == 0, "execute 1");
	if (strstr(out1, "Berlin") == NULL)
		fprintf(stderr, "dbg out1:%.800s\n", out1);
	CHECK(strstr(out1, "Berlin") != NULL, "Berlin substring");
	CHECK(strstr(out1, "99.6") != NULL, "hot max temp");
	CHECK(strstr(out1, "DayX") != NULL, "holiday name from HY_OK stub");
	CHECK(strstr(out1, "holiday_line") != NULL, "dashboard holiday_line populated");

	tool_context_test_set_http_bodies(GEO_OK, WX_B, HY_OK);

	CHECK(tool->execute("{}", out2, sizeof(out2)) == 0, "execute 2");
	CHECK(strstr(out2, "99.6") != NULL, "cache still hot");

	wall += (time_t)4000;
	tool_context_test_set_unix_time(wall);
	CHECK(tool->execute("{}", out3, sizeof(out3)) == 0, "execute 3 after TTL advance");
	CHECK(strstr(out3, "12.05") != NULL, "refetch cool max");

	tool_context_test_reset();
	fd = mkstemp(tmpl);
	CHECK(fd >= 0, "mkstemp failed");
	close(fd);
	CHECK(write_agent_fallback_toml(tmpl) == 0, "write cfg");
	CHECK(config_load(tmpl, &cfg, errbuf, sizeof(errbuf)) == 0, "config_load");

	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(cfg);
	tool_context_test_set_http_bodies(GEO_FAIL, WX_A, HY_OK);
	CHECK(tool_context_get()->execute("{}", out_fallback, sizeof(out_fallback)) == 0, "fallback exec");
	CHECK(strstr(out_fallback, "agent_fallback") != NULL, "expects agent_fallback source");
	CHECK(strstr(out_fallback, "\"country_code\":\"FR\"") != NULL, "expects FR from agent TOML");

	config_free(cfg);
	cfg = NULL;
	unlink(tmpl);

	tool_context_test_reset();

	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_FAIL, WX_A, HY_OK);
	CHECK(tool_context_get()->execute("{}", out_fallback, sizeof(out_fallback)) == 0,
	      "partial exec when geo fails without agent coords");
	CHECK(strstr(out_fallback, "\"available\":false") != NULL ||
		      strstr(out_fallback, "\"available\": false") != NULL,
	      "expects some unavailable slice");
	CHECK(strstr(out_fallback, "geolocation unavailable") != NULL,
	      "expects geo error text");

	tool_context_test_reset();
	{
		char tmpl_missing_lat[] = "/tmp/shellclaw_test_context_lat_XXXXXX";
		fd = mkstemp(tmpl_missing_lat);
		CHECK(fd >= 0, "mkstemp failed for missing-lat fallback");
		close(fd);
		CHECK(write_agent_fallback_toml(tmpl_missing_lat) == 0, "write cfg for missing-lat fallback");
		CHECK(config_load(tmpl_missing_lat, &cfg, errbuf, sizeof(errbuf)) == 0, "config_load missing-lat");
		tool_context_test_set_unix_time(t_march9_2027());
		tool_context_set_config(cfg);
		tool_context_test_set_http_bodies(GEO_MISSING_LAT, WX_A, HY_OK);
		CHECK(tool_context_get()->execute("{}", out_fallback, sizeof(out_fallback)) == 0,
		      "missing lat with agent coords falls back");
		CHECK(strstr(out_fallback, "agent_fallback") != NULL, "expects agent_fallback for missing lat");
		CHECK(strstr(out_fallback, "\"country_code\":\"FR\"") != NULL, "expects FR from agent TOML");
		config_free(cfg);
		cfg = NULL;
		unlink(tmpl_missing_lat);
	}

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_MISSING_LAT, WX_A, HY_OK);
	CHECK(tool_context_get()->execute("{}", out_fallback, sizeof(out_fallback)) == 0,
	      "missing lat without agent coords");
	CHECK(strstr(out_fallback, "\"available\":false") != NULL ||
		      strstr(out_fallback, "\"available\": false") != NULL,
	      "expects unavailable geo when lat missing and no agent coords");

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, WX_A, HY_OK);
	CHECK(tool_context_get()->execute("{}", out1, sizeof(out1)) == 0, "snapshot priming exec");
	CHECK(strstr(out1, "\"location_line\"") != NULL, "execute payload has location_line");
	CHECK(strstr(out1, "Berlin, DE") != NULL, "location_line shows city and country code");
	CHECK(strstr(out1, "\"weather_line\"") != NULL, "execute payload has weather_line");
	CHECK(strstr(out1, "99.6C") != NULL, "weather_line includes max temperature");
	CHECK(strstr(out1, "2027-03-09") != NULL, "weather_line includes reference day");
	CHECK(strstr(out1, "\"holiday_line\"") != NULL, "execute payload has holiday_line");
	CHECK(strstr(out1, "DayX") != NULL, "holiday_line includes next holiday name");
	CHECK(strstr(out1, "2027-03-10") != NULL, "holiday_line includes next holiday date");
	{
		char tiny[80];
		CHECK(tool_context_get()->execute("{}", tiny, sizeof(tiny)) == -1,
		      "tiny buffer must fail");
		CHECK(strstr(tiny, "too large") != NULL, "expects buffer size error");
	}
	{
		char *snap = tool_context_snapshot_json();
		CHECK(snap != NULL, "snapshot_json alloc");
		CHECK(strstr(snap, "Berlin, DE") != NULL, "snapshot location_line");
		CHECK(strstr(snap, "99.6C") != NULL, "snapshot weather_line");
		CHECK(strstr(snap, "next DayX on 2027-03-10") != NULL, "snapshot holiday_line");
		free(snap);
	}

	tool_context_test_reset();
	{
		char tmpl2[] = "/tmp/shellclaw_test_context2_XXXXXX";
		int fd2 = mkstemp(tmpl2);
		CHECK(fd2 >= 0, "mkstemp for minimal snapshot");
		close(fd2);
		CHECK(write_agent_fallback_toml(tmpl2) == 0, "write cfg for minimal snapshot");
		CHECK(config_load(tmpl2, &cfg, errbuf, sizeof(errbuf)) == 0, "config_load minimal");
		tool_context_set_config(cfg);
		{
			char *snap = tool_context_snapshot_json();
			CHECK(snap != NULL, "minimal snapshot alloc");
			CHECK(strstr(snap, "location_hint") != NULL, "minimal snapshot has location_hint");
			CHECK(strstr(snap, "48.8566") != NULL, "minimal snapshot shows agent latitude");
			free(snap);
		}
		config_free(cfg);
		cfg = NULL;
		unlink(tmpl2);
	}

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_dec31_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, WX_A, HY_NEW_YEAR);
	CHECK(tool_context_get()->execute("{}", out1, sizeof(out1)) == 0, "year-span execute");
	CHECK(strstr(out1, "Silvester") != NULL || strstr(out1, "2027-12-31") != NULL,
	      "year-span holidays include Dec 31 entry");
	CHECK(strstr(out1, "is_public_holiday_tomorrow") != NULL &&
		      (strstr(out1, "\"is_public_holiday_tomorrow\":true") != NULL ||
		       strstr(out1, "\"is_public_holiday_tomorrow\": true") != NULL),
	      "year-span marks Jan 1 as tomorrow holiday");

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, "{\"not\":\"forecast\"}", HY_OK);
	CHECK(tool_context_get()->execute("{}", out1, sizeof(out1)) == 0, "execute with bad weather JSON");
	CHECK(strstr(out1, "open-meteo parse failure") != NULL, "weather parse failure surfaced");

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	{
		char *cold = tool_context_snapshot_json();
		CHECK(cold != NULL, "cold snapshot alloc");
		CHECK(strstr(cold, "Dashboard cache empty") != NULL, "cold snapshot hints empty cache");
		free(cold);
	}


	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, WX_A, HY_BOTH);
	CHECK(tool_context_get()->execute("{}", out1, sizeof(out1)) == 0, "holiday flags exec");
	CHECK(strstr(out1, "\"is_public_holiday_today\":true") != NULL ||
		      strstr(out1, "\"is_public_holiday_today\": true") != NULL,
	      "expects today holiday flag");
	CHECK(strstr(out1, "\"is_public_holiday_tomorrow\":true") != NULL ||
		      strstr(out1, "\"is_public_holiday_tomorrow\": true") != NULL,
	      "expects tomorrow holiday flag");

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, WX_FAIL, HY_OK);
	CHECK(tool_context_get()->execute("{}", out2, sizeof(out2)) == 0, "weather fail exec");
	CHECK(strstr(out2, "open-meteo unavailable") != NULL, "expects weather API failure text");
	CHECK(strstr(out2, "\"available\":false") != NULL ||
		      strstr(out2, "\"available\": false") != NULL,
	      "expects weather unavailable flag");

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_BAD, WX_A, HY_OK);
	CHECK(tool_context_get()->execute("{}", out3, sizeof(out3)) == 0, "geo malformed exec");
	CHECK(strstr(out3, "geolocation unavailable") != NULL, "expects geo parse failure text");


	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, WX_A, HTTP_FAIL);
	CHECK(tool_context_get()->execute("{}", out3, sizeof(out3)) == 0, "holidays unavailable exec");
	CHECK(strstr(out3, "Berlin") != NULL, "geo present when holidays fail");
	CHECK(strstr(out3, "99.6") != NULL, "weather present when holidays fail");
	CHECK(strstr(out3, "nager fetch failed") != NULL, "expects holidays fetch error");

	tool_context_test_reset();
	tool_context_test_set_unix_time(t_march9_2027());
	tool_context_set_config(NULL);
	tool_context_test_set_http_bodies(GEO_OK, WX_A, HY_OK);
	{
		/* 64 bytes fits the snprintf error string but not a full merged context JSON. */
		char tiny[64];
		CHECK(tool_context_get()->execute("{}", tiny, sizeof(tiny)) != 0,
		      "small buffer must fail when payload exceeds capacity");
		CHECK(strstr(tiny, "get_context payload too large") != NULL,
		      "small buffer error names oversized payload");
	}
	CHECK(tool_context_get()->execute("{}", NULL, 0) == -1, "null buf zero len returns -1");
	CHECK(tool_context_get()->execute("{}", NULL, 128) == -1, "null buf non-zero len returns -1");
	{
		char out_guard[128];
		CHECK(tool_context_get()->execute("{}", out_guard, 0) == -1,
		      "zero max_len returns -1");
	}

	tool_context_test_reset();

	curl_global_cleanup();
	puts("test_context OK");
	return 0;
}
