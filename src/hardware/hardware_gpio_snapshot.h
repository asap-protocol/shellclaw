/**
 * @file hardware_gpio_snapshot.h
 * @brief Build JSON array of 40-pin header status for the Web UI.
 */

#ifndef SHELLCLAW_HARDWARE_GPIO_SNAPSHOT_H
#define SHELLCLAW_HARDWARE_GPIO_SNAPSHOT_H

#include "cJSON.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Append one object per physical pin (1–40) to @p pins_array.
 * @param errbuf Optional error message on failure.
 * @param errbufsz Size of @p errbuf.
 * @return 0 on success, -1 when no pin table is bound for the active board.
 */
int hardware_gpio_snapshot_fill(cJSON *pins_array, char *errbuf, size_t errbufsz);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_HARDWARE_GPIO_SNAPSHOT_H */
