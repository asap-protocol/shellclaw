/**
 * @file lws_compat.h
 * @brief Types required by system headers before libwebsockets on some platforms (macOS).
 */
#ifndef SHELLCLAW_GATEWAY_LWS_COMPAT_H
#define SHELLCLAW_GATEWAY_LWS_COMPAT_H

#include <sys/types.h>

#ifndef u_char
typedef unsigned char u_char;
#endif
#ifndef u_short
typedef unsigned short u_short;
#endif

#endif /* SHELLCLAW_GATEWAY_LWS_COMPAT_H */
