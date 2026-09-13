/**
 * @file ws.h
 * @brief WebSocket message queue and connection management for WebChat channel.
 */

#ifndef SHELLCLAW_GATEWAY_WS_H
#define SHELLCLAW_GATEWAY_WS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Max WebSocket text payload (matches RESPONSE_BUF_SIZE in dispatch.c). */
#define WS_TEXT_MAX (32 * 1024)

/** Opaque WebSocket connection handle (lws wsi cast to void*). */
typedef void *ws_conn_t;

/**
 * Register a WebSocket connection. Called when connection is established.
 *
 * @param conn_id Unique connection ID (e.g. incrementing integer).
 * @param wsi     libwebsockets wsi (stored as ws_conn_t).
 * @return 0 on success, -1 when the connection table is full.
 */
int ws_register_conn(int conn_id, ws_conn_t wsi);

/**
 * Unregister a WebSocket connection. Called when connection closes.
 *
 * @param conn_id Connection ID.
 */
void ws_unregister_conn(int conn_id);

/**
 * Push incoming message to queue for poll.
 *
 * @param conn_id   Connection ID.
 * @param text      Message text (copied).
 */
void ws_push_incoming(int conn_id, const char *text);

/**
 * Pop next message from queue. Blocks up to timeout_ms.
 *
 * @param session_id_out Buffer for session_id (e.g. "webchat:123").
 * @param session_size   Size of session_id_out.
 * @param text_out       Buffer for message text.
 * @param text_size      Size of text_out.
 * @param timeout_ms     Max wait in milliseconds.
 * @return 1 if message received, 0 if timeout, -1 on error.
 */
int ws_pop_incoming(char *session_id_out, size_t session_size,
                    char *text_out, size_t text_size, int timeout_ms);

/**
 * Send text to a WebSocket connection by session_id.
 * Message is queued and sent from the lws thread. Session ID format: "webchat:<conn_id>".
 *
 * @param session_id Recipient session ID.
 * @param text       Text to send.
 * @return 0 on success, non-zero on error.
 */
int ws_send_to(const char *session_id, const char *text);

/** Queue @p text for every connected WebSocket client. */
void ws_broadcast_text(const char *text);

/**
 * Dequeue next outgoing message for conn_id. Must be called from lws thread only.
 *
 * @param conn_id   Connection ID.
 * @param buf       Output buffer.
 * @param buf_size  Size of buf.
 * @param len_out   Receives actual length written.
 * @return 1 if message dequeued, 0 if none.
 */
int ws_dequeue_outgoing(int conn_id, char *buf, size_t buf_size, size_t *len_out);

/**
 * Check if more outgoing messages pending for conn_id. Must be called from lws thread only.
 */
int ws_has_pending_outgoing(int conn_id);

/**
 * Get next connection ID for new connections.
 *
 * @return New unique connection ID.
 */
int ws_next_conn_id(void);

/**
 * Signal WebSocket subsystem to shut down. Wakes any threads blocked
 * in ws_pop_incoming.
 */
void ws_shutdown_signal(void);

/**
 * Drain and free all queued messages, reset connection map.
 * Call after shutdown to release resources.
 */
void ws_cleanup(void);

/**
 * Set libwebsockets context for lws_write. Called by gateway on start.
 *
 * @param ctx lws_context (may be NULL).
 */
void ws_set_context(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_GATEWAY_WS_H */
