/**
 * @file webchat.h
 * @brief WebChat channel for paired devices and native WebSocket clients.
 */

#ifndef SHELLCLAW_CHANNEL_WEBCHAT_H
#define SHELLCLAW_CHANNEL_WEBCHAT_H

#include "channels/channel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBCHAT_DEVICE_PREFIX "webchat:device:"

/** Return the channel for paired devices and native WebSocket clients. */
const channel_t *channel_webchat_get(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_CHANNEL_WEBCHAT_H */
