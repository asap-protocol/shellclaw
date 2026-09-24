/**
 * @file discord_helpers.c
 * @brief Pure Discord helpers for routing rules and REST backoff (unit-tested).
 */

#include "channels/discord_helpers.h"
#include "channels/channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *discord_lifecycle_str(discord_lifecycle_t lc)
{
	switch (lc) {
	case DISCORD_LIFECYCLE_DISABLED:
		return "disabled";
	case DISCORD_LIFECYCLE_DISCONNECTED:
		return "disconnected";
	case DISCORD_LIFECYCLE_CONNECTING:
		return "connecting";
	case DISCORD_LIFECYCLE_CONNECTED:
		return "connected";
	case DISCORD_LIFECYCLE_RECONNECTING:
		return "reconnecting";
	default:
		return "unknown";
	}
}

int discord_helpers_allow_entry_equals(const char *allow_entry, const char *author_id)
{
	if (!allow_entry || !author_id || allow_entry[0] == '\0' || author_id[0] == '\0')
		return 0;
	return strcmp(allow_entry, author_id) == 0;
}

int discord_helpers_user_in_allowlist(const char *const *allowed, int count, const char *author_id)
{
	int i;
	if (!allowed || count <= 0 || !author_id || author_id[0] == '\0')
		return 0;
	for (i = 0; i < count; i++) {
		if (discord_helpers_allow_entry_equals(allowed[i], author_id))
			return 1;
	}
	return 0;
}

#define DISCORD_SESSION_PREFIX "discord:c:"

int discord_helpers_session_id_from_channel(const char *channel_id, char *out, size_t outsiz)
{
	size_t idlen;
	size_t need;
	if (!channel_id || channel_id[0] == '\0' || !out || outsiz == 0)
		return -1;
	idlen = strlen(channel_id);
	need = strlen(DISCORD_SESSION_PREFIX) + idlen + 1;
	if (need > outsiz)
		return -1;
	if (snprintf(out, outsiz, "%s%s", DISCORD_SESSION_PREFIX, channel_id) >= (int)outsiz)
		return -1;
	return 0;
}

int discord_helpers_mentions_include_bot(const cJSON *mentions, const char *bot_id)
{
	int sz;
	int i;
	if (!mentions || !cJSON_IsArray(mentions) || !bot_id || bot_id[0] == '\0')
		return 0;
	sz = cJSON_GetArraySize(mentions);
	for (i = 0; i < sz; i++) {
		cJSON *user = cJSON_GetArrayItem(mentions, i);
		cJSON *id;
		if (!cJSON_IsObject(user))
			continue;
		id = cJSON_GetObjectItem(user, "id");
		if (cJSON_IsString(id) && id->valuestring && strcmp(id->valuestring, bot_id) == 0)
			return 1;
	}
	return 0;
}

int discord_helpers_route_message_create(const cJSON *payload, const char *const *allowed,
                                         int allowed_count, const char *bot_user_id,
                                         char *session_out, size_t session_outsz)
{
	cJSON *author;
	cJSON *bot_flag;
	cJSON *aid_item;
	const char *author_id;
	cJSON *guild_id;
	int is_guild;
	cJSON *ch;
	if (!payload || !cJSON_IsObject(payload) || !session_out || session_outsz == 0)
		return -1;
	session_out[0] = '\0';
	author = cJSON_GetObjectItem(payload, "author");
	if (!cJSON_IsObject(author))
		return 0;
	bot_flag = cJSON_GetObjectItem(author, "bot");
	if (cJSON_IsTrue(bot_flag))
		return 0;
	aid_item = cJSON_GetObjectItem(author, "id");
	if (!cJSON_IsString(aid_item) || !aid_item->valuestring || aid_item->valuestring[0] == '\0')
		return 0;
	author_id = aid_item->valuestring;
	if (!discord_helpers_user_in_allowlist(allowed, allowed_count, author_id))
		return 0;
	guild_id = cJSON_GetObjectItem(payload, "guild_id");
	is_guild = cJSON_IsString(guild_id) && guild_id->valuestring && guild_id->valuestring[0] != '\0';
	if (is_guild) {
		cJSON *mentions;
		if (!bot_user_id || bot_user_id[0] == '\0')
			return 0;
		mentions = cJSON_GetObjectItem(payload, "mentions");
		if (!discord_helpers_mentions_include_bot(mentions, bot_user_id))
			return 0;
	}
	ch = cJSON_GetObjectItem(payload, "channel_id");
	if (!cJSON_IsString(ch) || !ch->valuestring || ch->valuestring[0] == '\0')
		return 0;
	if (discord_helpers_session_id_from_channel(ch->valuestring, session_out, session_outsz) != 0)
		return 0;
	return 1;
}

int discord_helpers_send_backoff_ms(int attempt, double retry_after_sec, int jitter_raw,
                                    int base_ms, int cap_ms)
{
	int sleep_ms;
	if (retry_after_sec > 0.0)
		sleep_ms = (int)(retry_after_sec * 1000.0) + (jitter_raw % 50);
	else
		sleep_ms = (1 << attempt) * base_ms + jitter_raw;
	if (sleep_ms < 100)
		sleep_ms = 100;
	if (sleep_ms > cap_ms)
		sleep_ms = cap_ms;
	return sleep_ms;
}

int discord_helpers_rx_append(char **buf, size_t *len, size_t *cap, const void *in,
                              size_t chunk_len, size_t max_cap)
{
	size_t need;

	if (!buf || !len || !cap || (!in && chunk_len != 0) || max_cap < 1)
		return -1;
	if (chunk_len > max_cap || *len > max_cap - chunk_len)
		return -1;
	need = *len + chunk_len + 1;
	if (need > max_cap)
		return -1;
	if (*cap < need) {
		size_t ncap = *cap ? *cap * 2 : 4096;
		char *p;

		while (ncap < need)
			ncap *= 2;
		if (ncap > max_cap)
			ncap = max_cap;
		if (ncap < need)
			return -1;
		p = realloc(*buf, ncap);
		if (!p)
			return -1;
		*buf = p;
		*cap = ncap;
	}
	if (chunk_len > 0)
		memcpy(*buf + *len, in, chunk_len);
	*len += chunk_len;
	(*buf)[*len] = '\0';
	return 0;
}
