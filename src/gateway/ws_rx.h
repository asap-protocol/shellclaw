#ifndef SHELLCLAW_GATEWAY_WS_RX_H
#define SHELLCLAW_GATEWAY_WS_RX_H

#include "gateway/ws.h"

/** libwebsockets can deliver one message through multiple receive callbacks. */
#define WS_RX_BUFFER_SIZE ((size_t)WS_TEXT_MAX + 64u)
/** JSON can expand each byte to six bytes. Allow extra space for field names. */
#define WS_RX_JSON_MAX ((size_t)WS_TEXT_MAX * 6u + 64u)

typedef struct {
	char *data;
	size_t len;
	size_t capacity;
	int discarding;
} ws_rx_t;

/** The libwebsockets service thread owns this state. */
typedef struct {
	int conn_id;
	ws_rx_t rx;
} ws_session_t;

/** Release a partial message and reset @p rx. */
void ws_rx_clear(ws_rx_t *rx);

/** Feed a chunk into a zero-initialized receiver.
 * @return 1 for a complete message. The caller frees @p text_out.
 *         0 for an incomplete or discarded message, -1 on allocation failure.
 */
int ws_rx_feed(ws_rx_t *rx, const void *chunk, size_t len, int final, char **text_out);

#endif
