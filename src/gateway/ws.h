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
/** C-string dest bytes for a WS_TEXT_MAX payload, including the NUL. */
#define WS_TEXT_BUF_SIZE (WS_TEXT_MAX + 1)
/**
 * One libwebsockets receive spill for the ws protocol.
 * Not a whole-message cap: JSON escaping can outgrow this.
 */
#define WS_RX_SPILL ((size_t)WS_TEXT_MAX + 64u)
/**
 * Largest inbound WebChat JSON object to reassemble.
 * JSON.stringify expands one input byte to at most six (`\u00XX`),
 * and the `{"type":"message","text":...}` envelope fits in the slack.
 *
 * Example: a WS_TEXT_MAX string of backslashes is accepted; one byte more is not.
 */
#define WS_RX_JSON_MAX ((size_t)WS_TEXT_MAX * 6u + 64u)

/**
 * True when @p len_out bytes fit in a WS_TEXT_MAX on-wire text frame.
 * Inclusive so a full 32 KiB payload is written after dequeue, not dropped.
 *
 * Example: `if (ws_text_payload_fits(len_out)) lws_write(...)`
 */
static inline int ws_text_payload_fits(size_t len_out)
{
	return len_out <= (size_t)WS_TEXT_MAX;
}

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
 * Drops outgoing frames still queued for @p conn_id so a closed session
 * cannot fill the shared outbound queue.
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
 * Frames targeted at an absent, unregistered, or closed socket (no live wsi) are
 * dropped instead of queued and 0 is still returned, so a dead session cannot
 * pin the shared outgoing queue.
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
 * Ask the service thread to flush queued WebChat frames.
 * Call only from the libwebsockets thread, in
 * LWS_CALLBACK_EVENT_WAIT_CANCELLED. lws_callback_on_writable() is not
 * safe from the agent thread: libwebsockets 4.3 updates the poll set
 * without the per-thread lock.
 *
 * Example: `case LWS_CALLBACK_EVENT_WAIT_CANCELLED: ws_on_service_cancel(); break;`
 */
void ws_on_service_cancel(void);

/**
 * In-progress WebChat frame. libwebsockets spills one websocket message
 * across several LWS_CALLBACK_RECEIVE calls. The bytes live here until
 * lws_is_final_fragment() is true. Zero the struct before the first feed
 * and call ws_rx_acc_free() when the connection goes away.
 */
typedef struct ws_rx_acc {
	char *buf;
	size_t len;
	size_t cap;
	int skip;
} ws_rx_acc_t;

/** Drop any partial frame held in @p acc. Safe on a zeroed struct. */
void ws_rx_acc_free(ws_rx_acc_t *acc);

/**
 * Append one RX chunk. On the final chunk, a complete `type:message`
 * whose text fits in WS_TEXT_MAX is returned in @p text_out (malloc'd).
 *
 * @param acc      Accumulator for this connection.
 * @param in       Chunk bytes. May be NULL only when @p len is 0.
 * @param len      Chunk length.
 * @param is_final Non-zero when this chunk ends the websocket message.
 * @param text_out Receives the message text, or NULL when more chunks
 *                 are required or the message was dropped.
 * @return 1 when @p text_out is set, 0 when nothing is ready, -1 on OOM.
 *
 * Example: feed each LWS_CALLBACK_RECEIVE until the return value is 1.
 */
int ws_rx_acc_feed(ws_rx_acc_t *acc, const void *in, size_t len, int is_final,
                   char **text_out);

/**
 * Same as ws_rx_acc_feed() for a registered `conn_id`.
 * The accumulator is tracked with the connection, because lws_set_wsi_user()
 * stores the conn id in the only per-wsi slot libwebsockets gives us.
 *
 * @return 1 when @p text_out is set, 0 when nothing is ready or @p conn_id
 *         is not registered, -1 on OOM.
 *
 * Example: `ws_conn_rx_feed(conn_id, in, len, lws_is_final_fragment(wsi), &text);`
 */
int ws_conn_rx_feed(int conn_id, const void *in, size_t len, int is_final,
                    char **text_out);

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
