/**
 * @file hardware_i2c.h
 * @brief Linux /dev/i2c-N backend with injectable syscalls for unit tests.
 */

#ifndef SHELLCLAW_HARDWARE_I2C_H
#define SHELLCLAW_HARDWARE_I2C_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Linux ioctl numbers (stable); used by mocks on non-Linux test hosts. */
#define HARDWARE_I2C_IOCTL_SLAVE 0x0703u
#define HARDWARE_I2C_IOCTL_RDWR  0x0707u
#define HARDWARE_I2C_IOCTL_SMBUS 0x0720u

/** Injectable syscalls (defaults to POSIX open/read/write/ioctl on Linux). */
typedef struct hardware_i2c_syscalls {
	int (*open_fn)(const char *path, int flags);
	int (*close_fn)(int fd);
	ssize_t (*read_fn)(int fd, void *buf, size_t count);
	ssize_t (*write_fn)(int fd, const void *buf, size_t count);
	int (*ioctl_fn)(int fd, unsigned long request, void *arg);
} hardware_i2c_syscalls_t;

int hardware_i2c_init(void);
void hardware_i2c_shutdown(void);
int hardware_i2c_is_available(void);

/**
 * Read @p len bytes from @p reg at 7-bit @p addr on @p bus.
 * @param out Caller buffer (must hold @p len bytes).
 */
int hardware_i2c_read(int bus, uint8_t addr, uint8_t reg, size_t len, uint8_t *out,
		      char *errbuf, size_t errbufsz);

/** Write @p len bytes to @p reg at 7-bit @p addr on @p bus. */
int hardware_i2c_write(int bus, uint8_t addr, uint8_t reg, const uint8_t *data,
		       size_t len, char *errbuf, size_t errbufsz);

/**
 * Probe addresses 0x03–0x77; writes responding 7-bit addresses to @p out_addrs.
 * @param count_out Number of addresses written (may be zero).
 */
int hardware_i2c_scan(int bus, uint8_t *out_addrs, int max_addrs, int *count_out,
		      char *errbuf, size_t errbufsz);

/** Override syscalls for unit tests (NULL restores defaults). */
void hardware_i2c_set_syscalls_for_test(const hardware_i2c_syscalls_t *ops);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_HARDWARE_I2C_H */
