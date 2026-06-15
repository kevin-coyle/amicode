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

/* cJSON_AddStringToObject with Latin-1 -> UTF-8 conversion of the value
 * (see api_to_utf8: the API requires UTF-8, the Amiga is Latin-1). */
static void add_utf8_string(cJSON *obj, const char *key, const char *val)
{
    char *u = api_to_utf8(val);
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
    /* A file written via write_file is generated entirely as output tokens
     * inside the tool call, so a low cap truncates large writes (forcing
     * retries / temp-file workarounds). Default to the model's maximum output:
     * claude-sonnet-4-6 allows 64K, and streaming (which we always do) removes
     * the timeout concern. If you switch to an Opus model (128K) via
     * amicode.config, raise max_tokens there to use the extra headroom. */
    c->cfg.max_tokens = 64000;
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

/* Does `content` hold a *_tool_result block whose tool_use_id == id? */
static BOOL has_result_for(cJSON *content, const char *id)
{
    cJSON *b;
    cJSON_ArrayForEach(b, content)
    {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "type");
        if (cJSON_IsString(t) && strstr(t->valuestring, "tool_result"))
        {
            cJSON *tid = cJSON_GetObjectItemCaseSensitive(b, "tool_use_id");
            if (cJSON_IsString(tid) && strcmp(tid->valuestring, id) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

/* Drop any server_tool_use block (web_search/web_fetch) that has no matching
 * web_search_tool_result in the same assistant turn. A truncated stream or a
 * mishandled pause can otherwise leave an orphan that the API rejects forever
 * ("server_tool_use ... without a corresponding ..._tool_result block"),
 * poisoning every subsequent request. */
static void sanitize_server_tools(cJSON *content)
{
    cJSON *b;

    if (!cJSON_IsArray(content))
        return;
    b = content->child;
    while (b)
    {
        cJSON *next = b->next;
        cJSON *t    = cJSON_GetObjectItemCaseSensitive(b, "type");
        if (cJSON_IsString(t) && strcmp(t->valuestring, "server_tool_use") == 0)
        {
            cJSON *id = cJSON_GetObjectItemCaseSensitive(b, "id");
            if (!cJSON_IsString(id) || !has_result_for(content, id->valuestring))
            {
                cJSON_DetachItemViaPointer(content, b);
                cJSON_Delete(b);
            }
        }
        b = next;
    }
}

/* Self-heal: guarantee the API's invariant that every tool_use in an assistant
 * message has a matching tool_result in the next (user) message. Inject a
 * synthetic error result for any that are missing. Runs before every request,
 * so a malformed turn from ANY cause can't make every future request fail and
 * silently burn credits. A no-op on healthy history. */
static void repair_history(cJSON *messages)
{
    int i = 0;
    int count = cJSON_GetArraySize(messages);

    while (i < count)
    {
        cJSON *msg  = cJSON_GetArrayItem(messages, i);
        cJSON *role = cJSON_GetObjectItemCaseSensitive(msg, "role");
        cJSON *content, *next_content = NULL, *block, *missing = NULL;

        if (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0)
        { i++; continue; }
        content = cJSON_GetObjectItemCaseSensitive(msg, "content");
        if (!cJSON_IsArray(content))
        { i++; continue; }

        if (i + 1 < count)
        {
            cJSON *next  = cJSON_GetArrayItem(messages, i + 1);
            cJSON *nrole = cJSON_GetObjectItemCaseSensitive(next, "role");
            if (cJSON_IsString(nrole) && strcmp(nrole->valuestring, "user") == 0)
                next_content = cJSON_GetObjectItemCaseSensitive(next, "content");
        }

        cJSON_ArrayForEach(block, content)
        {
            cJSON *t  = cJSON_GetObjectItemCaseSensitive(block, "type");
            cJSON *id = cJSON_GetObjectItemCaseSensitive(block, "id");
            if (!cJSON_IsString(t) || strcmp(t->valuestring, "tool_use") != 0)
                continue;
            if (!cJSON_IsString(id))
                continue;
            if (cJSON_IsArray(next_content) &&
                has_result_for(next_content, id->valuestring))
                continue;

            if (!missing)
                missing = cJSON_CreateArray();
            {
                cJSON *tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", id->valuestring);
                cJSON_AddStringToObject(tr, "content",
                    "(no result was recorded for this tool call)");
                cJSON_AddBoolToObject(tr, "is_error", 1);
                cJSON_AddItemToArray(missing, tr);
            }
        }

        if (missing)
        {
            if (cJSON_IsArray(next_content))
            {
                cJSON *m;
                while ((m = missing->child) != NULL)
                {
                    cJSON_DetachItemViaPointer(missing, m);
                    cJSON_AddItemToArray(next_content, m);
                }
                cJSON_Delete(missing);
            }
            else
            {
                cJSON *um = cJSON_CreateObject();
                cJSON_AddStringToObject(um, "role", "user");
                cJSON_AddItemToObject(um, "content", missing);
                cJSON_InsertItemInArray(messages, i + 1, um);
                count++;
            }
        }
        i++;
    }
}

/* Start a new assistant turn in history from `response`, returning the stored
 * message object (so pause_turn continuations can be merged into it). */
static cJSON *begin_assistant_turn(struct Conversation *c, const cJSON *response)
{
    cJSON *content = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "content");
    cJSON *msg     = cJSON_CreateObject();
    if (!msg)
        return NULL;

    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddItemToObject(msg, "content",
        cJSON_IsArray(content) ? cJSON_Duplicate(content, 1) : cJSON_CreateArray());
    cJSON_AddItemToArray(c->messages, msg);
    return msg;
}

/* Merge a pause_turn continuation's content blocks into the existing turn so a
 * server_tool_use and its result stay in one assistant message. */
static void merge_into_assistant_turn(cJSON *msg, const cJSON *response)
{
    cJSON *dst = cJSON_GetObjectItemCaseSensitive(msg, "content");
    cJSON *src = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "content");
    cJSON *blk;

    if (!cJSON_IsArray(dst) || !cJSON_IsArray(src))
        return;
    cJSON_ArrayForEach(blk, src)
        cJSON_AddItemToArray(dst, cJSON_Duplicate(blk, 1));
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
    int    round;
    cJSON *assistant = NULL;   /* in-progress assistant turn (merged across pauses) */

    if (err_out)
        *err_out = NULL;

    for (round = 0; round < MAX_TOOL_ROUNDS; round++)
    {
        cJSON      *response;
        const char *apierr;
        cJSON      *stop;
        cJSON      *tool_msg;

        /* Belt-and-suspenders: never send a history with a dangling tool_use. */
        repair_history(c->messages);

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

        /* Start a new assistant turn, or merge a pause_turn continuation into
         * the existing one so a server_tool_use and its web_search_tool_result
         * stay in the SAME assistant message (the API requires them paired). */
        if (assistant == NULL)
            assistant = begin_assistant_turn(c, response);
        else
            merge_into_assistant_turn(assistant, response);

        /* Server-side code execution (web_search dynamic filtering) creates a
         * container; capture its id so a resume request can reuse it. */
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

        /* Server-side tools paused the turn after their iteration cap. Keep the
         * same assistant turn open and re-send to resume; the continuation's
         * result blocks will be merged into it next round. */
        if (cJSON_IsString(stop) && strcmp(stop->valuestring, "pause_turn") == 0)
        {
            if (c->stream) print_server_activity(response);
            else           print_text_blocks(response);
            cJSON_Delete(response);
            continue;
        }

        /* The assistant turn is now complete; drop any orphan server_tool_use
         * (e.g. from a truncated stream) before it can poison later requests. */
        if (assistant)
            sanitize_server_tools(
                cJSON_GetObjectItemCaseSensitive(assistant, "content"));
        assistant = NULL;

        /* Show narration, then decide by the PRESENCE of tool_use blocks rather
         * than trusting stop_reason: if the assistant called any client tool we
         * MUST send back a tool_result for each, or the API rejects every later
         * request ("tool_use ids without tool_result blocks"). run_tool_uses
         * returns NULL only when there are no client tool calls. */
        if (c->stream) print_server_activity(response);
        else           print_text_blocks(response);

        tool_msg = run_tool_uses(response);
        if (tool_msg)
        {
            cJSON_Delete(response);
            cJSON_AddItemToArray(c->messages, tool_msg);
            continue;
        }

        /* No client tool calls -> this is the final answer. */
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
