/*
 * Liu accepts a user-supplied base URL for translation, voice transcription
 * and model catalog requests to OpenAI-compatible servers. These requests use
 * curl, whose supported protocols and redirects can access local files or
 * internal services unless restricted. Imported or shared configuration can
 * provide a URL, so accept only HTTP or HTTPS URLs with a non-empty host and
 * apply the same restriction to redirects.
 */
#ifndef CORE_API_URL_H
#define CORE_API_URL_H

#include "core/types.h"
#include <string.h>
#include <strings.h>

static inline bool liu_api_base_url_ok(const char *url) {
    if (!url) return false;
    const char *rest = NULL;
    if (strncasecmp(url, "https://", 8) == 0)     rest = url + 8;
    else if (strncasecmp(url, "http://", 7) == 0) rest = url + 7;
    if (!rest || !rest[0] || rest[0] == '/' || rest[0] == '@') return false;
    for (const char *p = rest; *p && *p != '/' && *p != '?' && *p != '#'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '\\') return false;
    }
    return true;
}

/* The curl-config lines that pin the transport to what the URL was checked
 * against. Written into every `-K -` config that carries a user endpoint. */
#define LIU_API_CURL_PROTO_LINES "proto = \"=http,https\"\nproto-redir = \"=http,https\"\n"

#endif /* CORE_API_URL_H */
