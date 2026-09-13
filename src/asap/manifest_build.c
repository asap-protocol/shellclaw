/**
 * @file manifest_build.c
 * @brief ASAP Manifest JSON tree builders (unsigned manifest document).
 */
#define _POSIX_C_SOURCE 200809L

#include "asap/manifest_build.h"
#include "asap/manifest_profiles.h"
#include "core/config.h"
#include "core/skill.h"
#include "core/version.h"
#include "hardware/board_detect.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SKILL_NAMES 64
#define MAX_SKILL_DESC 512
#define MAX_ASAP_ENDPOINT 512
#define MANIFEST_ASAP_VERSION "2.1.0"
/* Upstream asap.crypto.models defaults (verified against asap-protocol 2.5.0):
 * the signed inner manifest must carry every field Manifest.model_dump() would
 * populate, since asap.crypto.signing.canonicalize re-derives these defaults. */
#define MANIFEST_DEFAULT_TTL_SECONDS 300
#define MANIFEST_DEFAULT_TRANSPORT_VERSION "2.2"

static int build_asap_endpoint(char *out, size_t out_size, const config_t *cfg)
{
	const char *base;
	size_t len;
	int n;

	if (!out || out_size == 0) return -1;
	base = config_asap_public_base_url(cfg);
	if (!base || base[0] == '\0') return -1;
	len = strlen(base);
	while (len > 0 && base[len - 1] == '/') len--;
	n = snprintf(out, out_size, "%.*s/asap", (int)len, base);
	return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}

static const char *skill_description_for(const config_t *cfg, const char *skill_id)
{
	static const struct {
		const char *id;
		const char *desc;
	} defaults[] = {
		{ "assistant", "General assistant on edge hardware" },
		{ "edge_briefing", "Local briefing with optional cloud fallback" },
		{ "server_admin", "Host administration tasks" },
		{ "gpio_control", "GPIO pin control" },
		{ NULL, NULL }
	};
	static char buf[MAX_SKILL_DESC];
	size_t i;
	const char *override;

	if (!skill_id || !skill_id[0]) return NULL;
	override = config_asap_skill_description(cfg, skill_id);
	if (override && override[0] != '\0') return override;
	if (cfg && skill_get_description(cfg, skill_id, buf, sizeof(buf)) == 0 && buf[0] != '\0')
		return buf;
	for (i = 0; defaults[i].id != NULL; i++) {
		if (strcmp(defaults[i].id, skill_id) == 0)
			return defaults[i].desc;
	}
	return skill_id;
}

/* Attach item to parent under key; on failure (key strdup OOM) the item is
 * not attached and must be freed by the caller. Returns 0 on success, -1 on
 * failure with item deleted. Avoids orphan-node leaks under OOM --
 * LeakSanitizer caught these on Linux CI when the test_manifest_build
 * alloc-failure sweep tripped the key strdup mid-build. */
static int cjson_add_item_to_object_checked(cJSON *parent, const char *key,
					    cJSON *item)
{
	if (!cJSON_AddItemToObject(parent, key, item)) {
		cJSON_Delete(item);
		return -1;
	}
	return 0;
}

/* Append item to array; on failure the item is not attached and must be freed.
 * Returns 0 on success, -1 on failure with item deleted. */
static int cjson_add_item_to_array_checked(cJSON *array, cJSON *item)
{
	if (!cJSON_AddItemToArray(array, item)) {
		cJSON_Delete(item);
		return -1;
	}
	return 0;
}

static cJSON *cjson_add_string_to_object_checked(cJSON *parent, const char *key,
						 const char *value)
{
	cJSON *item = cJSON_CreateString(value);
	if (!item)
		return NULL;
	if (cjson_add_item_to_object_checked(parent, key, item) != 0)
		return NULL;	/* item already deleted by the helper */
	return parent;
}

static int count_skills_on_disk(const config_t *cfg)
{
	const char *dir_path;
	DIR *dir;
	struct dirent *ent;
	int total = 0;
	size_t len;

	if (!cfg)
		return 0;
	dir_path = config_skills_dir(cfg);
	if (!dir_path || dir_path[0] == '\0')
		return 0;
	dir = opendir(dir_path);
	if (!dir)
		return 0;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		len = strlen(ent->d_name);
		if (len <= 3 || strcmp(ent->d_name + len - 3, ".md") != 0)
			continue;
		total++;
	}
	closedir(dir);
	return total;
}

static int add_capabilities_skills(cJSON *capabilities, const config_t *cfg)
{
	cJSON *skills;
	char *names[MAX_SKILL_NAMES];
	int n;
	int total;
	int i;

	if (!capabilities) return -1;
	skills = cJSON_CreateArray();
	if (!skills) return -1;
	if (cjson_add_item_to_object_checked(capabilities, "skills", skills) != 0)
		return -1;	/* skills orphan deleted by helper; root cleaned by caller */
	if (!cfg) return 0;
	total = count_skills_on_disk(cfg);
	n = skill_list_names(cfg, names, MAX_SKILL_NAMES);
	if (n < 0) return -1;
	if (total > MAX_SKILL_NAMES)
		fprintf(stderr, "manifest: skills truncated (%d > %d)\n", total,
			MAX_SKILL_NAMES);
	for (i = 0; i < n && i < MAX_SKILL_NAMES; i++) {
		cJSON *item;
		cJSON *id_str;
		cJSON *desc_str;
		const char *desc;
		item = cJSON_CreateObject();
		if (!item) {
			for (int j = i; j < n; j++)
				free(names[j]);
			return -1;
		}
		if (cjson_add_item_to_array_checked(skills, item) != 0) {
			/* item orphan deleted by helper */
			for (int j = i; j < n; j++)
				free(names[j]);
			return -1;
		}
		id_str = cJSON_CreateString(names[i]);
		if (!id_str) {
			cJSON_DeleteItemFromArray(skills, cJSON_GetArraySize(skills) - 1);
			for (int j = i; j < n; j++)
				free(names[j]);
			return -1;
		}
		if (cjson_add_item_to_object_checked(item, "id", id_str) != 0) {
			/* id_str orphan deleted by helper; remove the partial skill entry */
			cJSON_DeleteItemFromArray(skills, cJSON_GetArraySize(skills) - 1);
			for (int j = i; j < n; j++)
				free(names[j]);
			return -1;
		}
		desc = skill_description_for(cfg, names[i]);
		desc_str = cJSON_CreateString(desc ? desc : names[i]);
		if (!desc_str) {
			cJSON_DeleteItemFromArray(skills, cJSON_GetArraySize(skills) - 1);
			for (int j = i; j < n; j++)
				free(names[j]);
			return -1;
		}
		if (cjson_add_item_to_object_checked(item, "description", desc_str) != 0) {
			/* desc_str orphan deleted by helper; remove the partial skill entry */
			cJSON_DeleteItemFromArray(skills, cJSON_GetArraySize(skills) - 1);
			for (int j = i; j < n; j++)
				free(names[j]);
			return -1;
		}
		/* Upstream Skill model carries input_schema/output_schema (default
		 * null); emit them so the signed tree matches Manifest.model_dump(). */
		{
			cJSON *input_schema = cJSON_CreateNull();
			cJSON *output_schema = cJSON_CreateNull();
			if (!input_schema || !output_schema) {
				if (input_schema) cJSON_Delete(input_schema);
				if (output_schema) cJSON_Delete(output_schema);
				for (int j = i; j < n; j++)
					free(names[j]);
				return -1;
			}
			if (cjson_add_item_to_object_checked(item, "input_schema",
							     input_schema) != 0) {
				/* input_schema orphan deleted by helper; output_schema still
				 * unattached and must be freed explicitly. */
				cJSON_Delete(output_schema);
				cJSON_DeleteItemFromArray(skills,
							  cJSON_GetArraySize(skills) - 1);
				for (int j = i; j < n; j++)
					free(names[j]);
				return -1;
			}
			if (cjson_add_item_to_object_checked(item, "output_schema",
							     output_schema) != 0) {
				/* output_schema orphan deleted by helper; input_schema already
				 * attached and freed with the skill entry below. */
				cJSON_DeleteItemFromArray(skills,
							  cJSON_GetArraySize(skills) - 1);
				for (int j = i; j < n; j++)
					free(names[j]);
				return -1;
			}
		}
		free(names[i]);
	}
	return 0;
}

static int add_capabilities_hardware(cJSON *capabilities, const config_t *cfg, board_id_t board)
{
	cJSON *hardware;
	cJSON *io_arr;
	const manifest_board_profile_t *profile;
	const char *class_name;
	const char *model_name;
	int io_count;
	int i;

	profile = manifest_board_profile(board);
	class_name = config_hardware_class(cfg);
	if (!class_name || class_name[0] == '\0') class_name = profile->class_name;
	model_name = config_hardware_model(cfg);
	if (!model_name || model_name[0] == '\0') model_name = profile->model_name;
	hardware = cJSON_CreateObject();
	if (!hardware) return -1;
	if (cjson_add_item_to_object_checked(capabilities, "hardware", hardware) != 0)
		return -1;	/* hardware orphan deleted by helper; root cleaned by caller */
	if (!cjson_add_string_to_object_checked(hardware, "class_", class_name))
		return -1;
	if (!cjson_add_string_to_object_checked(hardware, "model", model_name))
		return -1;
	io_arr = cJSON_CreateArray();
	if (!io_arr) {
		cJSON_DeleteItemFromObject(capabilities, "hardware");
		return -1;
	}
	if (cjson_add_item_to_object_checked(hardware, "io", io_arr) != 0) {
		/* io_arr orphan deleted by helper; drop the partial hardware subtree */
		cJSON_DeleteItemFromObject(capabilities, "hardware");
		return -1;
	}
	io_count = config_hardware_io_count(cfg);
	if (io_count > 0) {
		for (i = 0; i < io_count; i++) {
			cJSON *io_str;
			const char *entry = config_hardware_io_entry(cfg, i);
			if (!entry || entry[0] == '\0')
				continue;
			io_str = cJSON_CreateString(entry);
			if (!io_str) {
				cJSON_DeleteItemFromObject(capabilities, "hardware");
				return -1;
			}
			if (cjson_add_item_to_array_checked(io_arr, io_str) != 0) {
				/* io_str orphan deleted by helper; drop hardware subtree */
				cJSON_DeleteItemFromObject(capabilities, "hardware");
				return -1;
			}
		}
	} else {
		for (i = 0; i < profile->io_count; i++) {
			cJSON *io_str = cJSON_CreateString(profile->io[i]);
			if (!io_str) {
				cJSON_DeleteItemFromObject(capabilities, "hardware");
				return -1;
			}
			if (cjson_add_item_to_array_checked(io_arr, io_str) != 0) {
				/* io_str orphan deleted by helper; drop hardware subtree */
				cJSON_DeleteItemFromObject(capabilities, "hardware");
				return -1;
			}
		}
	}
	return 0;
}

static int add_capabilities_inference(cJSON *capabilities, board_id_t board)
{
	cJSON *inference;
	cJSON *modes;
	cJSON *local_models;
	cJSON *model_entry;
	const manifest_board_profile_t *profile;
	int i;

	profile = manifest_board_profile(board);
	inference = cJSON_CreateObject();
	if (!inference) return -1;
	if (cjson_add_item_to_object_checked(capabilities, "inference", inference) != 0)
		return -1;	/* inference orphan deleted by helper; root cleaned by caller */
	modes = cJSON_CreateArray();
	if (!modes) {
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	if (cjson_add_item_to_object_checked(inference, "modes", modes) != 0) {
		/* modes orphan deleted by helper; drop the partial inference subtree */
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	for (i = 0; i < profile->mode_count; i++) {
		cJSON *mode_str = cJSON_CreateString(profile->modes[i]);
		if (!mode_str) {
			cJSON_DeleteItemFromObject(capabilities, "inference");
			return -1;
		}
		if (cjson_add_item_to_array_checked(modes, mode_str) != 0) {
			/* mode_str orphan deleted by helper; drop inference subtree */
			cJSON_DeleteItemFromObject(capabilities, "inference");
			return -1;
		}
	}
	local_models = cJSON_CreateArray();
	if (!local_models) {
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	if (cjson_add_item_to_object_checked(inference, "local_models",
					     local_models) != 0) {
		/* local_models orphan deleted by helper; drop inference subtree */
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	model_entry = cJSON_CreateObject();
	if (!model_entry) {
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	if (cjson_add_item_to_array_checked(local_models, model_entry) != 0) {
		/* model_entry orphan deleted by helper; drop inference subtree */
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	if (!cjson_add_string_to_object_checked(model_entry, "id", profile->local_model_id)) {
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	if (!cjson_add_string_to_object_checked(model_entry, "quantization",
						profile->local_quantization)) {
		cJSON_DeleteItemFromObject(capabilities, "inference");
		return -1;
	}
	/* Upstream LocalModelInfo carries throughput_tokens_per_second (default
	 * null); emit it so the signed tree matches Manifest.model_dump(). */
	{
		cJSON *throughput = cJSON_CreateNull();
		if (!throughput) {
			cJSON_DeleteItemFromObject(capabilities, "inference");
			return -1;
		}
		if (cjson_add_item_to_object_checked(model_entry,
						     "throughput_tokens_per_second",
						     throughput) != 0) {
			/* throughput orphan deleted by helper; drop inference subtree */
			cJSON_DeleteItemFromObject(capabilities, "inference");
			return -1;
		}
	}
	return 0;
}

cJSON *manifest_build_tree(const config_t *cfg)
{
	cJSON *root;
	cJSON *capabilities;
	cJSON *endpoints;
	cJSON *item;
	char asap_url[MAX_ASAP_ENDPOINT];
	board_id_t board;
	const char *urn;
	const char *name;

	root = cJSON_CreateObject();
	if (!root) return NULL;
	urn = config_asap_agent_urn(cfg);
	name = config_asap_agent_name(cfg);
	board = manifest_resolve_board(cfg);
	item = cJSON_CreateString(urn);
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(root, "id", item) != 0) {
		cJSON_Delete(root);
		return NULL;	/* item orphan deleted by helper */
	}
	item = cJSON_CreateString(name);
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(root, "name", item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	item = cJSON_CreateString(SHELLCLAW_RELEASE_VERSION);
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(root, "version", item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	item = cJSON_CreateString(config_asap_description(cfg));
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(root, "description", item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	capabilities = cJSON_CreateObject();
	if (!capabilities) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(root, "capabilities", capabilities) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	item = cJSON_CreateString(MANIFEST_ASAP_VERSION);
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(capabilities, "asap_version", item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	if (add_capabilities_skills(capabilities, cfg) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	item = cJSON_CreateFalse();
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(capabilities, "state_persistence",
					     item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	item = cJSON_CreateFalse();
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(capabilities, "streaming", item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	item = cJSON_CreateArray();
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(capabilities, "mcp_tools", item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	if (add_capabilities_hardware(capabilities, cfg, board) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	if (add_capabilities_inference(capabilities, board) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	endpoints = cJSON_CreateObject();
	if (!endpoints) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(root, "endpoints", endpoints) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	if (build_asap_endpoint(asap_url, sizeof(asap_url), cfg) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	item = cJSON_CreateString(asap_url);
	if (!item) {
		cJSON_Delete(root);
		return NULL;
	}
	if (cjson_add_item_to_object_checked(endpoints, "asap", item) != 0) {
		cJSON_Delete(root);
		return NULL;
	}
	/* Upstream Endpoint model carries events (default null); emit it so the
	 * signed tree matches Manifest.model_dump(). */
	{
		cJSON *events = cJSON_CreateNull();
		if (!events) {
			cJSON_Delete(root);
			return NULL;
		}
		if (cjson_add_item_to_object_checked(endpoints, "events", events) != 0) {
			cJSON_Delete(root);
			return NULL;
		}
	}
	/* Top-level defaults populated by Manifest.model_dump(exclude={"signature"}):
	 * supported_versions, auth, sla, verification, ttl_seconds. Emit them so the
	 * signed bytes are byte-identical to asap.crypto.signing.canonicalize. */
	{
		cJSON *supported = cJSON_CreateArray();
		cJSON *ver_str;
		cJSON *auth;
		cJSON *sla;
		cJSON *verification;
		cJSON *ttl;

		if (!supported) {
			cJSON_Delete(root);
			return NULL;
		}
		if (cjson_add_item_to_object_checked(root, "supported_versions",
						     supported) != 0) {
			cJSON_Delete(root);
			return NULL;
		}
		ver_str = cJSON_CreateString(MANIFEST_DEFAULT_TRANSPORT_VERSION);
		if (!ver_str) {
			cJSON_Delete(root);
			return NULL;
		}
		if (cjson_add_item_to_array_checked(supported, ver_str) != 0) {
			cJSON_Delete(root);
			return NULL;
		}
		auth = cJSON_CreateNull();
		if (!auth) {
			cJSON_Delete(root);
			return NULL;
		}
		if (cjson_add_item_to_object_checked(root, "auth", auth) != 0) {
			cJSON_Delete(root);
			return NULL;
		}
		sla = cJSON_CreateNull();
		if (!sla) {
			cJSON_Delete(root);
			return NULL;
		}
		if (cjson_add_item_to_object_checked(root, "sla", sla) != 0) {
			cJSON_Delete(root);
			return NULL;
		}
		verification = cJSON_CreateNull();
		if (!verification) {
			cJSON_Delete(root);
			return NULL;
		}
		if (cjson_add_item_to_object_checked(root, "verification",
						     verification) != 0) {
			cJSON_Delete(root);
			return NULL;
		}
		ttl = cJSON_CreateNumber((double)MANIFEST_DEFAULT_TTL_SECONDS);
		if (!ttl) {
			cJSON_Delete(root);
			return NULL;
		}
		if (cjson_add_item_to_object_checked(root, "ttl_seconds", ttl) != 0) {
			cJSON_Delete(root);
			return NULL;
		}
	}
	return root;
}

char *manifest_build_json(const config_t *cfg)
{
	cJSON *root;
	char *out;
	root = manifest_build_tree(cfg);
	if (!root)
		return NULL;
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	return out;
}
