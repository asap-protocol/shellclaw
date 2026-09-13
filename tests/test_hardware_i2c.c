/**
 * @file test_hardware_i2c.c
 * @brief hardware_i2c backend tests with injectable syscall vtable.
 */

#include "test_runner.h"
#include "hardware/hardware_i2c.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct mock_i2c_state {
	char last_open_path[32];
	int open_fail;
	int slave_addr;
	int slave_fail;
	uint8_t last_write[64];
	size_t last_write_len;
	int read_fail;
	uint8_t read_payload[16];
	size_t read_payload_len;
	int probe_fail;
	int close_count;
} mock_i2c_state_t;

static mock_i2c_state_t s_mock;

static int mock_open(const char *path, int flags)
{
	(void)flags;
	if (s_mock.open_fail) {
		errno = ENOENT;
		return -1;
	}
	snprintf(s_mock.last_open_path, sizeof(s_mock.last_open_path), "%s", path);
	return 42;
}

static int mock_close(int fd)
{
	(void)fd;
	s_mock.close_count++;
	return 0;
}

static ssize_t mock_read(int fd, void *buf, size_t count)
{
	size_t n;
	(void)fd;
	if (s_mock.read_fail) {
		errno = EIO;
		return -1;
	}
	n = count;
	if (n > s_mock.read_payload_len)
		n = s_mock.read_payload_len;
	memcpy(buf, s_mock.read_payload, n);
	return (ssize_t)n;
}

static ssize_t mock_write(int fd, const void *buf, size_t count)
{
	(void)fd;
	if (count > sizeof(s_mock.last_write))
		count = sizeof(s_mock.last_write);
	memcpy(s_mock.last_write, buf, count);
	s_mock.last_write_len = count;
	return (ssize_t)count;
}

static int mock_ioctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (request == HARDWARE_I2C_IOCTL_SLAVE) {
		if (s_mock.slave_fail) {
			errno = EIO;
			return -1;
		}
		s_mock.slave_addr = (int)(uintptr_t)arg;
		return 0;
	}
	if (request == HARDWARE_I2C_IOCTL_SMBUS) {
		if (s_mock.probe_fail) {
			errno = ENXIO;
			return -1;
		}
		return 0;
	}
	errno = EINVAL;
	return -1;
}

static const hardware_i2c_syscalls_t s_mock_ops = {
	.open_fn = mock_open,
	.close_fn = mock_close,
	.read_fn = mock_read,
	.write_fn = mock_write,
	.ioctl_fn = mock_ioctl,
};

static int mock_reset(void)
{
	memset(&s_mock, 0, sizeof(s_mock));
	s_mock.probe_fail = 1;
	hardware_i2c_set_syscalls_for_test(&s_mock_ops);
	ASSERT(hardware_i2c_init() == 0);
	return 0;
}

static void mock_teardown(void)
{
	hardware_i2c_shutdown();
	hardware_i2c_set_syscalls_for_test(NULL);
}

static int test_i2c_read_register_pattern(void)
{
	uint8_t out[2] = { 0, 0 };
	char errbuf[128];
	ASSERT(mock_reset() == 0);
	s_mock.read_payload[0] = 0xAB;
	s_mock.read_payload[1] = 0xCD;
	s_mock.read_payload_len = 2;
	ASSERT(hardware_i2c_read(7, 0x76, 0xF7, 2, out, errbuf, sizeof(errbuf)) == 0);
	ASSERT(strcmp(s_mock.last_open_path, "/dev/i2c-7") == 0);
	ASSERT(s_mock.slave_addr == 0x76);
	ASSERT(s_mock.last_write_len == 1);
	ASSERT(s_mock.last_write[0] == 0xF7);
	ASSERT(out[0] == 0xAB);
	ASSERT(out[1] == 0xCD);
	mock_teardown();
	return 0;
}

static int test_i2c_write_register_pattern(void)
{
	const uint8_t payload[] = { 0x10, 0x20 };
	char errbuf[128];
	ASSERT(mock_reset() == 0);
	ASSERT(hardware_i2c_write(1, 0x23, 0xE0, payload, 2, errbuf, sizeof(errbuf)) == 0);
	ASSERT(strcmp(s_mock.last_open_path, "/dev/i2c-1") == 0);
	ASSERT(s_mock.slave_addr == 0x23);
	ASSERT(s_mock.last_write_len == 3);
	ASSERT(s_mock.last_write[0] == 0xE0);
	ASSERT(s_mock.last_write[1] == 0x10);
	ASSERT(s_mock.last_write[2] == 0x20);
	mock_teardown();
	return 0;
}

static int test_i2c_open_error_propagation(void)
{
	char errbuf[128];
	uint8_t out = 0;
	ASSERT(mock_reset() == 0);
	s_mock.open_fail = 1;
	ASSERT(hardware_i2c_read(7, 0x76, 0x00, 1, &out, errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "/dev/i2c-7") != NULL);
	mock_teardown();
	return 0;
}

static int test_i2c_slave_error_propagation(void)
{
	char errbuf[128];
	uint8_t out = 0;
	ASSERT(mock_reset() == 0);
	s_mock.slave_fail = 1;
	ASSERT(hardware_i2c_read(7, 0x76, 0x00, 1, &out, errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "I2C_SLAVE") != NULL);
	ASSERT(strstr(errbuf, "0x76") != NULL);
	mock_teardown();
	return 0;
}

static int test_i2c_scan_empty_bus(void)
{
	uint8_t addrs[8];
	int count = -1;
	char errbuf[128];
	ASSERT(mock_reset() == 0);
	ASSERT(hardware_i2c_scan(7, addrs, 8, &count, errbuf, sizeof(errbuf)) == 0);
	ASSERT(count == 0);
	ASSERT(strcmp(s_mock.last_open_path, "/dev/i2c-7") == 0);
	mock_teardown();
	return 0;
}

static int test_i2c_scan_finds_device(void)
{
	uint8_t addrs[4];
	int count = -1;
	char errbuf[128];
	ASSERT(mock_reset() == 0);
	s_mock.probe_fail = 0;
	ASSERT(hardware_i2c_scan(7, addrs, 4, &count, errbuf, sizeof(errbuf)) == 0);
	ASSERT(count > 0);
	ASSERT(addrs[0] == 0x03);
	mock_teardown();
	return 0;
}

static int test_i2c_read_rejects_reserved_addr(void)
{
	char errbuf[128];
	uint8_t out = 0;

	ASSERT(mock_reset() == 0);
	ASSERT(hardware_i2c_read(7, 0x02, 0x00, 1, &out, errbuf, sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "0x02") != NULL);
	ASSERT(strstr(errbuf, "0x03-0x77") != NULL);
	ASSERT(s_mock.close_count == 0);
	mock_teardown();
	return 0;
}

static int test_i2c_write_rejects_oversize_len(void)
{
	uint8_t payload[257];
	char errbuf[128];

	memset(payload, 0x11, sizeof(payload));
	ASSERT(mock_reset() == 0);
	ASSERT(hardware_i2c_write(1, 0x50, 0x00, payload, sizeof(payload), errbuf,
				   sizeof(errbuf)) != 0);
	ASSERT(strstr(errbuf, "1-256") != NULL);
	ASSERT(s_mock.close_count == 0);
	mock_teardown();
	return 0;
}

int main(void)
{
	RUN(test_i2c_read_register_pattern());
	RUN(test_i2c_write_register_pattern());
	RUN(test_i2c_open_error_propagation());
	RUN(test_i2c_slave_error_propagation());
	RUN(test_i2c_scan_empty_bus());
	RUN(test_i2c_scan_finds_device());
	RUN(test_i2c_read_rejects_reserved_addr());
	RUN(test_i2c_write_rejects_oversize_len());
	printf("test_hardware_i2c: all tests passed\n");
	return 0;
}
