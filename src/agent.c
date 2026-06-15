/* agent.c - conversation state + tool-use loop for amicode. */

#include <proto/dos.h>

#include <string.h>
#include <stdlib.h>

#include "agent.h"
#include "api.h"
#include "tools.h"

/* Safety bound on tool round-trips within a single user turn. Generous enough
 * for multi-step tasks; still bounds a runaway loop. */
#define MAX_TOOL_ROUNDS  100

/* The Anthropic API requires the JSON request body to be valid UTF-8, but the
 * Amiga is a Latin-1 (ISO-8859-1) machine: file contents, command output and
 * keyboard input routinely contain bytes >= 0x80 that are not valid UTF-8.
 * Convert to UTF-8, passing through byte runs that are already valid UTF-8 and
 * encoding stray high bytes as Latin-1. Returns a malloc'd string (caller
 * frees), or NULL on allocation failure. */
static char *to_utf8(const char *in)
{
    const unsigned char *p = (const unsigned char *)in;
    LONG  n = (LONG)strlen(in), i = 0, o = 0;
    char *out = malloc(n * 2 + 1);

    if (!out)
        return NULL;

    while (i < n)
    {
        unsigned char c = p[i];
        int len = 0;

        if (c < 0x80) { out[o++] = (char)c; i++; continue; }

        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;

        if (len && i + len <= n)
        {
            int k, ok = 1;
            for (k = 1; k < len; k++)
                if ((p[i + k] & 0xC0) != 0x80) { ok = 0; break; }
            if (ok)
            {
                for (k = 0; k < len; k++) out[o++] = (char)p[i + k];
                i += len;
                continue;
            }
        }

        /* Not valid UTF-8 here: treat the byte as Latin-1. */
        out[o++] = (char)(0xC0 | (c >> 6));
        out[o++] = (char)(0x80 | (c & 0x3F));
        i++;
    }
    out[o] = '\0';
    return out;
}

/* cJSON_AddStringToObject with Latin-1 -> UTF-8 conversion of the value. */
static void add_utf8_string(cJSON *obj, const char *key, const char *val)
{
    char *u = to_utf8(val);
    cJSON_AddStringToObject(obj, key, u ? u : val);
    if (u) free(u);
}

BOOL conv_init(struct Conversation *c, const char *api_key, const char *system)
{
    memset(c, 0, sizeof(*c));

    if (!(c->messages = cJSON_CreateArray()))
        return FALSE;

    c->tools = tools_definitions();   /* may be NULL; then we run tool-less */

    c->cfg.api_key    = api_key;
    c->cfg.model      = API_DEFAULT_MODEL;
    c->cfg.max_tokens = 2048;
    c->cfg.system     = system;
    c->cfg.tools      = c->tools;
    c->stream         = TRUE;
    return TRUE;
}

void conv_free(struct Conversation *c)
{
    if (c->messages) { cJSON_Delete(c->messages); c->messages = NULL; }
    if (c->tools)    { cJSON_Delete(c->tools);    c->tools = NULL; }
}

BOOL conv_add_user_text(struct Conversation *c, const char *text)
{
    cJSON *msg = cJSON_CreateObject();
    if (!msg)
        return FALSE;

    cJSON_AddStringToObject(msg, "role", "user");
    add_utf8_string(msg, "content", text);
    cJSON_AddItemToArray(c->messages, msg);
    return TRUE;
}

/* Append the assistant's reply to the history, preserving its full content
 * array (text and tool_use blocks) so the model sees its own turns. */
static void append_assistant_turn(struct Conversation *c, const cJSON *response)
{
    cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "content");
    cJSON *msg     = cJSON_CreateObject();
    if (!msg)
        return;

    cJSON_AddStringToObject(msg, "role", "assistant");
    if (cJSON_IsArray(content))
        cJSON_AddItemToObject(msg, "content", cJSON_Duplicate(content, 1));
    else
        cJSON_AddStringToObject(msg, "content", "");
    cJSON_AddItemToArray(c->messages, msg);
}

/* Print a one-line note for each server-side tool use (web_search / web_fetch
 * run on Anthropic's servers, so they never reach our client tool handler). */
static void print_server_activity(const cJSON *response)
{
    cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "content");
    cJSON *block;

    if (!cJSON_IsArray(content))
        return;
    cJSON_ArrayForEach(block, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        if (cJSON_IsString(type) &&
            strcmp(type->valuestring, "server_tool_use") == 0)
        {
            cJSON *name  = cJSON_GetObjectItemCaseSensitive(block, "name");
            cJSON *input = cJSON_GetObjectItemCaseSensitive(block, "input");
            cJSON *query = cJSON_GetObjectItemCaseSensitive(input, "query");
            cJSON *url   = cJSON_GetObjectItemCaseSensitive(input, "url");
            const char *arg = cJSON_IsString(query) ? query->valuestring
                            : cJSON_IsString(url)   ? url->valuestring : "";
            Printf("  -> %s %s\n",
                   (LONG)(cJSON_IsString(name) ? name->valuestring : "web"),
                   (LONG)arg);
        }
    }
}

/* Non-streaming narration: print text blocks, then server-tool activity. */
static void print_text_blocks(const cJSON *response)
{
    cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "content");
    cJSON *block;

    if (cJSON_IsArray(content))
    {
        cJSON_ArrayForEach(block, content)
        {
            cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
            cJSON *text = cJSON_GetObjectItemCaseSensitive(block, "text");
            if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 &&
                cJSON_IsString(text) && text->valuestring[0])
                Printf("%s\n", (LONG)text->valuestring);
        }
    }
    print_server_activity(response);
}

/* Execute every tool_use block in `response` and build the follow-up user
 * message of tool_result blocks. Returns the new message (added to history by
 * the caller), or NULL if there were no tool_use blocks. */
static cJSON *run_tool_uses(const cJSON *response)
{
    cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "content");
    cJSON *block, *results, *msg;
    int n_tools = 0;

    if (!cJSON_IsArray(content))
        return NULL;

    results = cJSON_CreateArray();

    cJSON_ArrayForEach(block, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        cJSON *id, *name, *input, *result_block;
        char *out;
        BOOL is_error = FALSE;

        if (!cJSON_IsString(type) || strcmp(type->valuestring, "tool_use") != 0)
            continue;

        id    = cJSON_GetObjectItemCaseSensitive(block, "id");
        name  = cJSON_GetObjectItemCaseSensitive(block, "name");
        input = cJSON_GetObjectItemCaseSensitive(block, "input");
        n_tools++;

        Printf("  -> %s\n", (LONG)(cJSON_IsString(name) ? name->valuestring : "?"));

        out = tools_execute(cJSON_IsString(name) ? name->valuestring : "",
                            input, &is_error);

        result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        if (cJSON_IsString(id))
            cJSON_AddStringToObject(result_block, "tool_use_id", id->valuestring);
        add_utf8_string(result_block, "content", out ? out : "(no result)");
        if (is_error)
            cJSON_AddBoolToObject(result_block, "is_error", 1);
        cJSON_AddItemToArray(results, result_block);

        if (out) free(out);
    }

    if (n_tools == 0)
    {
        cJSON_Delete(results);
        return NULL;
    }

    msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddItemToObject(msg, "content", results);
    return msg;
}

char *conv_run_turn(struct Conversation *c, const char **err_out)
{
    int round;

    if (err_out)
        *err_out = NULL;

    for (round = 0; round < MAX_TOOL_ROUNDS; round++)
    {
        cJSON      *response;
        const char *apierr;
        cJSON      *stop;
        cJSON      *tool_msg;

        response = c->stream ? api_create_message_streaming(&c->cfg, c->messages)
                             : api_create_message(&c->cfg, c->messages);
        if (!response)
        {
            strcpy(c->last_error, "no response (transport or TLS failure)");
            if (err_out) *err_out = c->last_error;
            return NULL;
        }

        if ((apierr = api_error_message(response)) != NULL)
        {
            strncpy(c->last_error, apierr, sizeof(c->last_error) - 1);
            c->last_error[sizeof(c->last_error) - 1] = '\0';
            if (err_out) *err_out = c->last_error;
            cJSON_Delete(response);
            return NULL;
        }

        append_assistant_turn(c, response);

        /* Server-side code execution (web_search/web_fetch dynamic filtering)
         * creates a container; capture its id so the resume request can reuse
         * it — otherwise the API rejects the follow-up ("container_id is
         * required when there are pending tool uses"). */
        {
            const char *cid = api_response_container_id(response);
            if (cid && cid[0] && strcmp(cid, c->container) != 0)
            {
                strncpy(c->container, cid, sizeof(c->container) - 1);
                c->container[sizeof(c->container) - 1] = '\0';
                c->cfg.container = c->container;
            }
        }

        /* When streaming, the assistant text has already been printed live;
         * close its line. (Non-streaming prints text below via print_*.) */
        if (c->stream)
            Printf("\n");

        stop = cJSON_GetObjectItemCaseSensitive(response, "stop_reason");

        /* Server-side tools (web_search/web_fetch) paused the turn after the
         * built-in iteration cap. The assistant turn (with its server_tool_use
         * + result blocks) is already in history; just re-send to resume. */
        if (cJSON_IsString(stop) && strcmp(stop->valuestring, "pause_turn") == 0)
        {
            if (c->stream) print_server_activity(response);
            else           print_text_blocks(response);
            cJSON_Delete(response);
            continue;
        }

        if (cJSON_IsString(stop) && strcmp(stop->valuestring, "tool_use") == 0)
        {
            /* Show any narration, run the tools, feed results back, loop. */
            if (c->stream) print_server_activity(response);
            else           print_text_blocks(response);
            tool_msg = run_tool_uses(response);
            cJSON_Delete(response);

            if (!tool_msg)
            {
                strcpy(c->last_error, "stop_reason was tool_use but no tool calls found");
                if (err_out) *err_out = c->last_error;
                return NULL;
            }
            cJSON_AddItemToArray(c->messages, tool_msg);
            continue;
        }

        /* Final turn: return the assistant's text. */
        {
            char *text = api_response_text(response);
            cJSON_Delete(response);
            return text;
        }
    }

    strcpy(c->last_error, "tool loop exceeded max rounds");
    if (err_out) *err_out = c->last_error;
    return NULL;
}
