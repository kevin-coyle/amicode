/* api.h - Anthropic Messages API request/response handling for amicode.
 *
 * Native Anthropic wire format (not the OpenAI-compat shim). Tool use is
 * first-class here and gets wired in at Phase 4; Phase 2/3 use the text path.
 *
 * Requests are built from a cJSON array of message objects so the same call
 * serves a one-shot prompt (Phase 2), a full history (Phase 3), and
 * tool_use/tool_result turns (Phase 4).
 */
#ifndef AMICODE_API_H
#define AMICODE_API_H

#include <exec/types.h>
#include "json/cJSON.h"

#define API_HOST            "api.anthropic.com"
#define API_PATH            "/v1/messages"
#define API_VERSION         "2023-06-01"
#define API_DEFAULT_MODEL   "claude-sonnet-4-6"

struct ApiConfig
{
    const char *api_key;     /* x-api-key value (required)            */
    const char *model;       /* e.g. "claude-sonnet-4-6"              */
    LONG        max_tokens;  /* response token cap                    */
    const char *system;      /* system prompt, or NULL               */
    cJSON      *tools;       /* tools array (referenced, not owned), or NULL */
    const char *container;   /* code-execution container id to reuse, or NULL */
};

/* POST one Messages request. `messages` is a cJSON array of {role,content}
 * objects; it is referenced, not consumed (caller still owns/frees it).
 *
 * On success returns the parsed JSON response object (caller cJSON_Delete()s
 * it) — this is the raw Anthropic response, including assistant `content`
 * blocks and `stop_reason`, or an `{type:"error",...}` object. Returns NULL on
 * a transport/TLS/parse failure (diagnostics already printed). */
cJSON *api_create_message(const struct ApiConfig *cfg, cJSON *messages);

/* Same as api_create_message, but streams the response: assistant text is
 * printed live (word-wrapped) to the Shell as it arrives, and the returned
 * object is reconstructed from the SSE events (same shape, incl. tool_use
 * blocks and stop_reason). Caller cJSON_Delete()s it. NULL on transport
 * failure; an error object on a non-2xx response. */
cJSON *api_create_message_streaming(const struct ApiConfig *cfg, cJSON *messages);

/* If `response` is an API error object, return its human-readable message
 * (points into `response`; do not free). Otherwise NULL. */
const char *api_error_message(const cJSON *response);

/* Return the code-execution container id from a response (`container.id`, or a
 * bare string `container`), or NULL. Points into `response`; do not free. */
const char *api_response_container_id(const cJSON *response);

/* Convert Latin-1 (ISO-8859-1) text to valid UTF-8 for the request body: byte
 * runs that are already valid UTF-8 pass through unchanged, stray high bytes
 * are encoded as Latin-1. Idempotent on valid UTF-8. Returns a malloc'd string
 * (caller frees), or NULL on allocation failure. */
char *api_to_utf8(const char *in);

/* Concatenate the text of every `text` block in an assistant response's
 * `content` array. Returns a malloc'd string (caller frees), or NULL if there
 * is no text content. */
char *api_response_text(const cJSON *response);

#endif /* AMICODE_API_H */
