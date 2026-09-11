/**
 * @file hardware_i2c.c
 * @brief /dev/i2c-N access via ioctl(I2C_SLAVE) with testable syscall vtable.
 */
#define _POSIX_C_SOURCE 200809L

#include "hardware/hardware_i2c.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#endif

#define I2C_ADDR_MIN 0x03u
#define I2C_ADDR_MAX 0x77u
#define I2C_XFER_LEN_MAX 256u

static const hardware_i2c_syscalls_t *s_test_syscalls;
static int s_i2c_ready;

static int default_open(const char *path, int flags)
{
	return open(path, flags);
}

static int default_close(int fd)
{
	return close(fd);
}

static ssize_t default_read(int fd, void *buf, size_t count)
{
	return read(fd, buf, count);
}

static ssize_t default_write(int fd, const void *buf, size_t count)
{
	return write(fd, buf, count);
}

static int default_ioctl(int fd, unsigned long request, void *arg)
{
#if defined(__linux__)
	if (request == HARDWARE_I2C_IOCTL_SLAVE)
		return ioctl(fd, I2C_SLAVE, (unsigned long)(uintptr_t)arg);
	if (request == HARDWARE_I2C_IOCTL_RDWR)
		return ioctl(fd, I2C_RDWR, arg);
	if (request == HARDWARE_I2C_IOCTL_SMBUS)
		return ioctl(fd, I2C_SMBUS, arg);
#endif
	(void)fd;
	(void)request;
	(void)arg;
	errno = ENOSYS;
	return -1;
}

static const hardware_i2c_syscalls_t s_default_syscalls = {
	.open_fn = default_open,
	.close_fn = default_close,
	.read_fn = default_read,
	.write_fn = default_write,
	.ioctl_fn = default_ioctl,
};

void hardware_i2c_set_syscalls_for_test(const hardware_i2c_syscalls_t *ops)
{
	s_test_syscalls = ops;
}

static const hardware_i2c_syscalls_t *active_syscalls(void)
{
	if (s_test_syscalls != NULL)
		return s_test_syscalls;
	return &s_default_syscalls;
}

static int bus_path(int bus, char *path, size_t pathsz)
{
	int n = snprintf(path, pathsz, "/dev/i2c-%d", bus);
	if (n < 0 || (size_t)n >= pathsz)
		return -1;
	return 0;
}

static int open_bus(int bus, char *errbuf, size_t errbufsz)
{
	const hardware_i2c_syscalls_t *ops = active_syscalls();
	char path[32];
	int fd;
	if (bus < 0 || bus > 255) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: bus %d out of range (0-255)", bus);
		return -1;
	}
	if (bus_path(bus, path, sizeof(path)) != 0) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: bus path too long for bus %d", bus);
		return -1;
	}
	fd = ops->open_fn(path, O_RDWR);
	if (fd < 0) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: open %s failed: %s", path,
				 strerror(errno));
		return -1;
	}
	return fd;
}

static int set_slave(int fd, uint8_t addr, char *errbuf, size_t errbufsz)
{
	const hardware_i2c_syscalls_t *ops = active_syscalls();
	uintptr_t arg = (uintptr_t)addr;
	if (ops->ioctl_fn(fd, HARDWARE_I2C_IOCTL_SLAVE, (void *)arg) != 0) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: I2C_SLAVE 0x%02x failed: %s", addr,
				 strerror(errno));
		return -1;
	}
	return 0;
}

#if defined(__linux__)
/* addr is already selected via I2C_SLAVE in set_slave() before this is called;
 * probe_address just issues an I2C_SMBUS_QUICK write to the current slave. */
static int probe_address(int fd, uint8_t addr)
{
	const hardware_i2c_syscalls_t *ops = active_syscalls();
	if (s_test_syscalls == NULL) {
		union i2c_smbus_data smbus_data;
		struct i2c_smbus_ioctl_data args;
		memset(&smbus_data, 0, sizeof(smbus_data));
		memset(&args, 0, sizeof(args));
		args.read_write = I2C_SMBUS_WRITE;
		args.command = 0;
		args.size = I2C_SMBUS_QUICK;
		args.data = &smbus_data;
		(void)addr;
		return ops->ioctl_fn(fd, HARDWARE_I2C_IOCTL_SMBUS, &args);
	}
	(void)addr;
	return ops->ioctl_fn(fd, HARDWARE_I2C_IOCTL_SMBUS, NULL);
}
#else
static int probe_address(int fd, uint8_t addr)
{
	const hardware_i2c_syscalls_t *ops = active_syscalls();
	if (s_test_syscalls != NULL) {
		(void)addr;
		return ops->ioctl_fn(fd, HARDWARE_I2C_IOCTL_SMBUS, NULL);
	}
	(void)fd;
	(void)addr;
	errno = ENODEV;
	return -1;
}
#endif

int hardware_i2c_init(void)
{
	s_i2c_ready = 1;
	return 0;
}

void hardware_i2c_shutdown(void)
{
	s_test_syscalls = NULL;
	s_i2c_ready = 0;
}

int hardware_i2c_is_available(void)
{
	return s_i2c_ready ? 1 : 0;
}

static int validate_i2c_xfer(uint8_t addr, size_t len, char *errbuf, size_t errbufsz)
{
	if (addr < I2C_ADDR_MIN || addr > I2C_ADDR_MAX) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: addr 0x%02x out of range (0x03-0x77)",
				 addr);
		return -1;
	}
	if (len < 1 || len > I2C_XFER_LEN_MAX) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: len %zu out of range (1-256)", len);
		return -1;
	}
	return 0;
}

int hardware_i2c_read(int bus, uint8_t addr, uint8_t reg, size_t len, uint8_t *out,
		      char *errbuf, size_t errbufsz)
{
	const hardware_i2c_syscalls_t *ops;
	int fd;
	ssize_t n;
	if (!s_i2c_ready) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: backend not initialized");
		return -1;
	}
	if (!out) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: invalid read buffer or len=%zu", len);
		return -1;
	}
	if (validate_i2c_xfer(addr, len, errbuf, errbufsz) != 0)
		return -1;
	fd = open_bus(bus, errbuf, errbufsz);
	if (fd < 0)
		return -1;
	if (set_slave(fd, addr, errbuf, errbufsz) != 0) {
		ops = active_syscalls();
		ops->close_fn(fd);
		return -1;
	}
	ops = active_syscalls();
	n = ops->write_fn(fd, &reg, 1);
	if (n != 1) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: write reg 0x%02x failed: %s", reg,
				 strerror(errno));
		ops->close_fn(fd);
		return -1;
	}
	n = ops->read_fn(fd, out, len);
	if ((size_t)n != len) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz,
				 "i2c: read %zu bytes at 0x%02x reg 0x%02x failed: %s", len,
				 addr, reg, strerror(errno));
		ops->close_fn(fd);
		return -1;
	}
	ops->close_fn(fd);
	return 0;
}

int hardware_i2c_write(int bus, uint8_t addr, uint8_t reg, const uint8_t *data,
		       size_t len, char *errbuf, size_t errbufsz)
{
	const hardware_i2c_syscalls_t *ops;
	uint8_t *buf = NULL;
	size_t total;
	ssize_t n;
	int fd;
	int ret = -1;
	if (!s_i2c_ready) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: backend not initialized");
		return -1;
	}
	if (!data) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: invalid write buffer or len=%zu", len);
		return -1;
	}
	if (validate_i2c_xfer(addr, len, errbuf, errbufsz) != 0)
		return -1;
	fd = open_bus(bus, errbuf, errbufsz);
	if (fd < 0)
		return -1;
	if (set_slave(fd, addr, errbuf, errbufsz) != 0) {
		ops = active_syscalls();
		ops->close_fn(fd);
		return -1;
	}
	total = 1 + len;
	buf = malloc(total);
	if (!buf) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: out of memory for write buffer");
		ops = active_syscalls();
		ops->close_fn(fd);
		return -1;
	}
	buf[0] = reg;
	memcpy(buf + 1, data, len);
	ops = active_syscalls();
	n = ops->write_fn(fd, buf, total);
	if ((size_t)n != total) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz,
				 "i2c: write %zu bytes at 0x%02x reg 0x%02x failed: %s", len,
				 addr, reg, strerror(errno));
		goto done;
	}
	ret = 0;
done:
	free(buf);
	ops->close_fn(fd);
	return ret;
}

int hardware_i2c_scan(int bus, uint8_t *out_addrs, int max_addrs, int *count_out,
		      char *errbuf, size_t errbufsz)
{
	const hardware_i2c_syscalls_t *ops;
	int fd;
	int count = 0;
	uint8_t addr;
	if (!s_i2c_ready) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: backend not initialized");
		return -1;
	}
	if (!count_out) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: count_out is NULL");
		return -1;
	}
	*count_out = 0;
	if (max_addrs > 0 && !out_addrs) {
		if (errbuf && errbufsz > 0)
			snprintf(errbuf, errbufsz, "i2c: out_addrs is NULL with max_addrs=%d",
				 max_addrs);
		return -1;
	}
	fd = open_bus(bus, errbuf, errbufsz);
	if (fd < 0)
		return -1;
	for (addr = I2C_ADDR_MIN; addr <= I2C_ADDR_MAX; addr++) {
		if (max_addrs > 0 && count >= max_addrs)
			break;
		if (set_slave(fd, addr, NULL, 0) != 0)
			continue;
		if (probe_address(fd, addr) < 0)
			continue;
		if (max_addrs > 0)
			out_addrs[count] = addr;
		count++;
	}
	ops = active_syscalls();
	ops->close_fn(fd);
	*count_out = count;
	return 0;
}
