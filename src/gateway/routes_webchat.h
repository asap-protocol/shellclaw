/**
 * @file routes_webchat.h
 * @brief WebChat routes for an authenticated device.
 */
#ifndef SHELLCLAW_ROUTES_WEBCHAT_H
#define SHELLCLAW_ROUTES_WEBCHAT_H

#include "channels/webchat.h"
#include "gateway/http_lws.h"
#include "gateway/ws.h"

#define WEBCHAT_ROUTE "/api/chat"
#define WEBCHAT_SESSION_SIZE (sizeof(WEBCHAT_DEVICE_PREFIX) + 64)
#define WEBCHAT_RESPONSE_SIZE (12u * WS_TEXT_MAX + 1024u)

/** Derive the session ID from the validated bearer credential in the HTTP request. */
int webchat_request_session(struct lws *wsi, char session[WEBCHAT_SESSION_SIZE]);

/** Handle a history or send request after the gateway authenticates the device. */
void handle_webchat(struct lws *wsi, int method, const char *body, size_t body_len,
                    char *buf, size_t size, int *status);

#endif
