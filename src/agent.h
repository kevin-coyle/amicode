/* agent.h - conversation state + turn loop for amicode.
 *
 * Phase 3: holds the running message history and drives one request/response
 * turn against the Messages API. Phase 4 extends conv_run_turn() into the full
 * tool-use loop (execute tool_use blocks, feed tool_result back, repeat until
 * stop_reason == "end_turn").
 */
#ifndef AMICODE_AGENT_H
#define AMICODE_AGENT_H

#include <exec/types.h>
#include "api.h"
#include "json/cJSON.h"

struct Conversation
{
    cJSON            *messages;       /* JSON array of {role,content} turns */
    cJSON            *tools;          /* tool definitions (owned here)      */
    struct ApiConfig  cfg;            /* api_key/model/system/max_tokens    */
    BOOL              stream;         /* stream responses live (default on) */
    char              container[80]; /* code-exec container id to reuse     */
    char              last_error[256];/* message from the most recent error */
};

/* Initialise an empty conversation. `api_key` and `system` are borrowed
 * (must outlive the conversation). Returns FALSE on allocation failure. */
BOOL conv_init(struct Conversation *c, const char *api_key, const char *system);

/* Free the message history (not the borrowed api_key/system). */
void conv_free(struct Conversation *c);

/* Append a plain-text user turn to the history. */
BOOL conv_add_user_text(struct Conversation *c, const char *text);

/* Run one turn: send the full history, append the assistant's reply (its whole
 * content array) to the history, and return the assistant's text as a malloc'd
 * string (caller frees). Returns NULL on error; if the API returned an error
 * object, *err_out (if non-NULL) points to its message (valid until the next
 * turn). */
char *conv_run_turn(struct Conversation *c, const char **err_out);

#endif /* AMICODE_AGENT_H */
