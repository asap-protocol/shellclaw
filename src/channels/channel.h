/**
 * @file channel.h
 * @brief Channel vtable and message types. CLI and Telegram implement this interface.
 */

#ifndef SHELLCLAW_CHANNEL_H
#define SHELLCLAW_CHANNEL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct config;
typedef struct config config_t;

/** Attachment for incoming (photo) or outgoing (optional) messages. */
typedef struct channel_attachment {
	char *path_or_base64;  /**< File path or base64-encoded image data */
	size_t size;           /**< Length of path_or_base64 string */
	int is_base64;         /**< 1 if base64, 0 if file path */
} channel_attachment_t;

/**
 * Incoming message from a channel. Channel allocates; caller must call
 * channel_incoming_msg_clear() when done.
 */
typedef struct channel_incoming_msg {
	char *session_id;           /**< e.g. "cli:default", "telegram:123456" */
	char *user_id;              /**< Channel-specific user ID (e.g. Telegram user id) */
	char *text;                 /**< Message text (may be empty for photo-only) */
	channel_attachment_t *attachments;
	size_t attachments_count;
} channel_incoming_msg_t;

/** Clear and free incoming message fields. Safe to call repeatedly or on zeroed. */
void channel_incoming_msg_clear(channel_incoming_msg_t *msg);

/**
 * Byte length of the next outbound piece of @p text.
 * The piece is at most @p max_bytes and stops before a multibyte UTF-8
 * character that would not fit, so chat APIs do not see a split code point.
 * Fallback: when the first rune is wider than @p max_bytes, the piece is
 * @p max_bytes bytes (that rune is split); unreachable with the 2000/4096
 * channel caps, documented so the finite cut is not mistaken for a bug.
 *
 * Example: `channel_outbound_chunk_len("hello", 2)` returns 2.
 *
 * @param text      NUL-terminated text. NULL returns 0.
 * @param max_bytes Maximum piece size in bytes. 0 returns 0.
 * @return Byte length of the next piece, or 0 when @p text is empty.
 */
size_t channel_outbound_chunk_len(const char *text, size_t max_bytes);

/**
 * Post one chunk of a channel_send_chunked() send. NUL-terminated piece,
 * never empty and never longer than the max_bytes of the driving call.
 * @param piece NUL-terminated text piece (valid until the callback returns).
 * @param ctx   Caller context passed through unchanged. May be NULL.
 * @return 0 when the platform accepted the piece, non-zero on failure.
 */
typedef int (*channel_chunk_post_fn)(const char *piece, void *ctx);

/** Position where a chunked send stopped; zeroed by the call unless it failed. */
typedef struct channel_chunk_fail {
	size_t index;   /**< Zero-based index of the chunk that failed to post. */
	size_t offset;  /**< Byte offset of the failed chunk inside the sent text. */
} channel_chunk_fail_t;

/**
 * Deliver @p text via @p post in chunks of at most @p max_bytes, cut on UTF-8
 * rune boundaries via channel_outbound_chunk_len(). Each piece is copied into
 * one internal buffer and NUL-terminated, so plain C-string platform posts
 * (Telegram sendMessage query, Discord JSON `content`) work unchanged.
 *
 * Limits are BYTES even where a platform counts characters (Telegram's 4096
 * is characters): conservative by design, so CJK/emoji-heavy replies split
 * earlier than strictly required. Do not "fix" this to count code points.
 *
 * Example: `channel_send_chunked(text, 2000, post_fn, &ctx, &fail)` posts up
 * to 2000-byte pieces until done; when `post_fn` returns non-zero the send
 * stops and `fail` reports the failing chunk index and byte offset.
 *
 * @param text      NUL-terminated text. NULL returns -1; empty posts nothing.
 * @param max_bytes Maximum chunk size in bytes. 0 returns -1.
 * @param post      Callback that delivers one piece; non-zero fails the send.
 * @param ctx       Caller context passed through to @p post. May be NULL.
 * @param fail      Optional out: chunk index and byte offset where the send
 *                  stopped. Zeroed when the call does not fail that way.
 * @return 0 when every chunk was posted, -1 on invalid arguments, OOM, or
 *         a failed @p post (with @p fail, when given, pointing at the chunk
 *         that failed).
 */
int channel_send_chunked(const char *text, size_t max_bytes,
                         channel_chunk_post_fn post, void *ctx,
                         channel_chunk_fail_t *fail);

/**
 * Channel vtable: init, poll, send, cleanup. CLI and Telegram implement this.
 */
typedef struct channel {
	const char *name;
	/** Initialize with config. Return 0 on success. */
	int (*init)(const config_t *cfg);
	/**
	 * Poll for incoming message. Block up to timeout_ms.
	 * @param out       Filled on success; caller must channel_incoming_msg_clear().
	 * @param timeout_ms Max wait in milliseconds.
	 * @return 1 if message received, 0 if timeout/no message, -1 on error.
	 */
	int (*poll)(channel_incoming_msg_t *out, int timeout_ms);
	/**
	 * Send response to recipient.
	 * @param recipient  Session ID or user ID (channel-specific).
	 * @param text       Response text (must not be NULL).
	 * @param attachments Optional; may be NULL if count is 0.
	 * @param att_count  Number of attachments.
	 * @return 0 on success, non-zero on error.
	 */
	int (*send)(const char *recipient, const char *text,
	            const channel_attachment_t *attachments, size_t att_count);
	/** Release channel resources. */
	void (*cleanup)(void);
} channel_t;

#define MAX_REGISTERED_CHANNELS 8

/** Register a channel by name for routing (e.g. cron send). */
void channel_register(const char *name, const channel_t *ch);

/** Get channel by name. Returns NULL if not found. */
const channel_t *channel_get_by_name(const char *name);

/** Stub channel for tests. poll() always returns 0 (no message). */
const channel_t *channel_stub_get(void);

/** CLI channel: stdin/stdout, -m one-shot, interactive prompt. */
const channel_t *channel_cli_get(void);

/** Set one-shot message for CLI (call before init when using -m). */
void channel_cli_set_one_shot(const char *msg);

/** Set verbose mode for CLI output (ANSI colors). */
void channel_cli_set_verbose(int v);

/** Telegram channel: long-poll getUpdates, allowlist, /reset, /status. */
const channel_t *channel_telegram_get(void);

/** Discord Gateway channel (libwebsockets client) when built with gateway. */
const channel_t *channel_discord_get(void);

/** SIGHUP live reload: Telegram allowlist/token env lookups use refreshed config pointers (no re-init). */
void shellclaw_telegram_set_live_cfg(const config_t *cfg);
/** Discord allowlist lookups use refreshed pointers (stub no-op without gateway client). */
void shellclaw_discord_set_live_cfg(const config_t *cfg);

/** Lifecycle snapshot for `/api/status` (non-blocking; no Gateway I/O). */
typedef enum discord_lifecycle {
	DISCORD_LIFECYCLE_DISABLED = 0,
	DISCORD_LIFECYCLE_DISCONNECTED,
	DISCORD_LIFECYCLE_CONNECTING,
	DISCORD_LIFECYCLE_CONNECTED,
	DISCORD_LIFECYCLE_RECONNECTING,
} discord_lifecycle_t;

/** Short diagnostic string for @ref discord_lifecycle_t ("disabled", "connected", …). */
const char *discord_lifecycle_str(discord_lifecycle_t lc);

typedef struct discord_status_snapshot {
	discord_lifecycle_t lifecycle;
	char reason[160];
} discord_status_snapshot_t;

/**
 * Fill Discord connection snapshot from config + channel runtime state.
 * Safe from HTTP handlers: bounded work, mutex snapshot only when runtime is active.
 */
void discord_status_snapshot_fill(const config_t *cfg, discord_status_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SHELLCLAW_CHANNEL_H */
