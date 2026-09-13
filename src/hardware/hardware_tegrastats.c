/**
 * @file hardware_tegrastats.c
 * @brief tegrastats single-shot collect + JetPack 6.x line parser.
 */
#define _POSIX_C_SOURCE 200809L

#include "hardware/hardware_tegrastats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define TEGRA_CMD "tegrastats --interval 100 --count 1 2>/dev/null"
#define NVPMODEL_CMD "nvpmodel -q 2>/dev/null"
#define LLAMA_PROCESS "llama-server"

static hardware_tegrastats_collect_fn s_test_collect;
static const char *s_test_power_mode;
static int s_test_llama_forced = -1;

void hardware_tegrastats_set_collect_for_test(hardware_tegrastats_collect_fn fn)
{
	s_test_collect = fn;
}

void hardware_tegrastats_set_power_mode_for_test(const char *mode)
{
	s_test_power_mode = mode;
}

void hardware_tegrastats_set_llama_running_for_test(int forced)
{
	s_test_llama_forced = forced;
}

static int parse_ram(const char *line, unsigned int *used, unsigned int *total)
{
	const char *p = strstr(line, "RAM ");
	unsigned int u;
	unsigned int t;

	if (!p)
		return -1;
	if (sscanf(p, "RAM %u/%uMB", &u, &t) != 2)
		return -1;
	*used = u;
	*total = t;
	return 0;
}

static int parse_gr3d(const char *line, unsigned int *usage, unsigned int *freq_mhz)
{
	const char *p = strstr(line, "GR3D_FREQ ");
	unsigned int u;
	unsigned int f1;
	unsigned int f2;

	if (!p)
		return -1;
	p += strlen("GR3D_FREQ ");
	if (sscanf(p, "%u%%@[%u,%u]", &u, &f1, &f2) == 3) {
		*usage = u;
		*freq_mhz = f1 > f2 ? f1 : f2;
		return 0;
	}
	if (sscanf(p, "%u%%@%u", &u, &f1) == 2) {
		*usage = u;
		*freq_mhz = f1;
		return 0;
	}
	return -1;
}

static int parse_gpu_temp(const char *line, float *temp_c)
{
	const char *p = strstr(line, "gpu@");
	float t;

	if (!p)
		p = strstr(line, "GPU@");
	if (!p)
		return -1;
	if (sscanf(p, "gpu@%fC", &t) != 1 && sscanf(p, "GPU@%fC", &t) != 1)
		return -1;
	*temp_c = t;
	return 0;
}

int hardware_tegrastats_parse_line(const char *line, hardware_tegrastats_parsed_t *out)
{
	float temp;

	if (!line || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	if (parse_ram(line, &out->ram_used_mb, &out->ram_total_mb) != 0)
		return -1;
	if (parse_gr3d(line, &out->gpu_usage_percent, &out->gpu_freq_mhz) != 0)
		return -1;
	if (parse_gpu_temp(line, &temp) == 0) {
		out->gpu_temp_c = temp;
		out->has_gpu_temp = 1;
	}
	return 0;
}

static int subprocess_ok(int wait_rc)
{
	if (wait_rc == -1)
		return 0;
	if (!WIFEXITED(wait_rc))
		return 0;
	return WEXITSTATUS(wait_rc) == 0;
}

static int default_collect(char *linebuf, size_t linebufsz, char *errbuf, size_t errbufsz)
{
	FILE *fp;
	char *nl;
	int wait_rc;

	if (!linebuf || linebufsz == 0) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "tegrastats: linebuf is NULL");
		return -1;
	}
	linebuf[0] = '\0';
	fp = popen(TEGRA_CMD, "r");
	if (!fp) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "tegrastats: popen failed");
		return -1;
	}
	if (!fgets(linebuf, (int)linebufsz, fp)) {
		pclose(fp);
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "tegrastats: no output");
		return -1;
	}
	wait_rc = pclose(fp);
	if (!subprocess_ok(wait_rc)) {
		linebuf[0] = '\0';
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "tegrastats: command failed");
		return -1;
	}
	nl = strchr(linebuf, '\n');
	if (nl)
		*nl = '\0';
	if (linebuf[0] == '\0') {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "tegrastats: empty line");
		return -1;
	}
	return 0;
}

int hardware_tegrastats_collect_line(char *linebuf, size_t linebufsz, char *errbuf,
				     size_t errbufsz)
{
	if (s_test_collect)
		return s_test_collect(linebuf, linebufsz, errbuf, errbufsz);
	return default_collect(linebuf, linebufsz, errbuf, errbufsz);
}

void hardware_tegrastats_read_power_mode(char *buf, size_t bufsz)
{
	FILE *fp;
	char line[256];
	const char *prefix = "NV Power Mode:";
	int wait_rc;

	if (!buf || bufsz == 0)
		return;
	buf[0] = '\0';
	if (s_test_power_mode) {
		snprintf(buf, bufsz, "%s", s_test_power_mode);
		return;
	}
	fp = popen(NVPMODEL_CMD, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		char *start = strstr(line, prefix);
		if (!start)
			continue;
		start += strlen(prefix);
		while (*start == ' ' || *start == '\t')
			start++;
		snprintf(buf, bufsz, "%s", start);
		{
			char *nl = strchr(buf, '\n');
			if (nl)
				*nl = '\0';
		}
		break;
	}
	wait_rc = pclose(fp);
	if (!subprocess_ok(wait_rc))
		buf[0] = '\0';
}

int hardware_llama_server_running(void)
{
	FILE *fp;
	char line[32];
	int wait_rc;

	if (s_test_llama_forced >= 0)
		return s_test_llama_forced ? 1 : 0;
	fp = popen("pgrep -x " LLAMA_PROCESS " 2>/dev/null", "r");
	if (!fp)
		return 0;
	if (!fgets(line, sizeof(line), fp))
		line[0] = '\0';
	wait_rc = pclose(fp);
	if (!subprocess_ok(wait_rc))
		return 0;
	return line[0] != '\0' ? 1 : 0;
}

static int merge_json_children(cJSON *root, cJSON *doc)
{
	cJSON *child;
	cJSON *next;

	if (!root || !doc)
		return -1;
	child = doc->child;
	while (child != NULL) {
		next = child->next;
		cJSON_DetachItemViaPointer(doc, child);
		cJSON_AddItemToObject(root, child->string, child);
		child = next;
	}
	return 0;
}

int hardware_jetson_gpu_json_fill(cJSON *root, char *errbuf, size_t errbufsz)
{
	char line[4096];
	hardware_tegrastats_parsed_t parsed;
	cJSON *doc = NULL;
	cJSON *llama = NULL;
	cJSON *status_item = NULL;
	unsigned int mem_pct = 0;
	int llama_on;
	int ret = -1;

	if (!root) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "gpu: root is NULL");
		return -1;
	}
	doc = cJSON_CreateObject();
	if (!doc) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "gpu: out of memory");
		return -1;
	}
	if (hardware_tegrastats_collect_line(line, sizeof(line), errbuf, errbufsz) != 0)
		goto fail;
	if (hardware_tegrastats_parse_line(line, &parsed) != 0) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "tegrastats: parse failed");
		goto fail;
	}
	hardware_tegrastats_read_power_mode(parsed.power_mode, sizeof(parsed.power_mode));
	if (parsed.ram_total_mb > 0)
		mem_pct = (unsigned int)((parsed.ram_used_mb * 100u) / parsed.ram_total_mb);
	cJSON_AddBoolToObject(doc, "available", 1);
	cJSON_AddNumberToObject(doc, "gpu_usage", (double)parsed.gpu_usage_percent);
	cJSON_AddNumberToObject(doc, "gpu_freq_mhz", (double)parsed.gpu_freq_mhz);
	cJSON_AddNumberToObject(doc, "memory_used_mb", (double)parsed.ram_used_mb);
	cJSON_AddNumberToObject(doc, "memory_total_mb", (double)parsed.ram_total_mb);
	cJSON_AddNumberToObject(doc, "memory_percent", (double)mem_pct);
	if (parsed.has_gpu_temp)
		cJSON_AddNumberToObject(doc, "temperature", (double)parsed.gpu_temp_c);
	if (parsed.power_mode[0]) {
		cJSON *power_mode = cJSON_CreateString(parsed.power_mode);
		if (!power_mode)
			goto fail;
		cJSON_AddItemToObject(doc, "power_mode", power_mode);
	}
	llama_on = hardware_llama_server_running();
	llama = cJSON_CreateObject();
	if (!llama)
		goto fail;
	cJSON_AddBoolToObject(llama, "running", llama_on ? 1 : 0);
	status_item = cJSON_CreateString(llama_on ? "running" : "stopped");
	if (!status_item)
		goto fail;
	cJSON_AddItemToObject(llama, "status", status_item);
	status_item = NULL;
	cJSON_AddItemToObject(doc, "llama_server", llama);
	llama = NULL;
	if (merge_json_children(root, doc) != 0)
		goto fail;
	ret = 0;
fail:
	if (status_item)
		cJSON_Delete(status_item);
	if (llama)
		cJSON_Delete(llama);
	if (doc)
		cJSON_Delete(doc);
	return ret;
}
