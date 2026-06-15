/* api.c - Anthropic Messages API request/response handling for amicode. */

#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "api.h"
#include "net.h"

char *api_to_utf8(const char *in)
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

        out[o++] = (char)(0xC0 | (c >> 6));
        out[o++] = (char)(0x80 | (c & 0x3F));
        i++;
    }
    out[o] = '\0';
    return out;
}

/* Build the "x-api-key / anthropic-version / content-type" header block. */
static char *build_headers(const char *api_key)
{
    LONG cap = (LONG)strlen(api_key) + 96;
    char *h = malloc(cap);
    if (!h)
        return NULL;
    sprintf(h,
        "x-api-key: %s\r\n"
        "anthropic-version: " API_VERSION "\r\n"
        "content-type: application/json\r\n",
        api_key);
    return h;
}

/* Build the request JSON body. messages/tools are referenced (caller owns). */
static char *build_body(const struct ApiConfig *cfg, cJSON *messages, BOOL stream)
{
    cJSON *root = cJSON_CreateObject();
    char  *body;

    if (!root)
        return NULL;

    cJSON_AddStringToObject(root, "model",
                            cfg->model ? cfg->model : API_DEFAULT_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens",
                            (double)(cfg->max_tokens > 0 ? cfg->max_tokens : 1024));
    if (cfg->system)
    {
        char *sys = api_to_utf8(cfg->system);
        cJSON_AddStringToObject(root, "system", sys ? sys : cfg->system);
        if (sys) free(sys);
    }
    cJSON_AddItemReferenceToObject(root, "messages", messages);
    if (cfg->tools)
        cJSON_AddItemReferenceToObject(root, "tools", cfg->tools);
    if (cfg->container && cfg->container[0])
        cJSON_AddStringToObject(root, "container", cfg->container);
    if (stream)
        cJSON_AddBoolToObject(root, "stream", 1);

    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

cJSON *api_create_message(const struct ApiConfig *cfg, cJSON *messages)
{
    char  *body    = NULL;
    char  *headers = NULL;
    struct HttpResponse resp;
    cJSON *parsed = NULL;

    if (!cfg || !cfg->api_key || !messages)
        return NULL;

    if (!(body = build_body(cfg, messages, FALSE)))
        return NULL;

    if (!(headers = build_headers(cfg->api_key)))
    {
        free(body);
        return NULL;
    }

    if (https_post(API_HOST, API_PATH, headers,
                   (const UBYTE *)body, (LONG)strlen(body), &resp))
    {
        if (resp.body)
            parsed = cJSON_Parse((const char *)resp.body);
        if (!parsed)
            Printf("amicode: couldn't parse API response (HTTP %ld)\n",
                   resp.status);
        http_response_free(&resp);
    }

    free(headers);
    free(body);
    return parsed;
}

const char *api_error_message(const cJSON *response)
{
    cJSON *type, *error, *msg;

    type = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "type");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "error") != 0)
        return NULL;

    error = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "error");
    msg   = cJSON_GetObjectItemCaseSensitive(error, "message");
    if (cJSON_IsString(msg))
        return msg->valuestring;
    return "unknown API error";
}

const char *api_response_container_id(const cJSON *response)
{
    cJSON *c = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "container");
    if (cJSON_IsObject(c))
    {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(c, "id");
        if (cJSON_IsString(id))
            return id->valuestring;
    }
    else if (cJSON_IsString(c))
    {
        return c->valuestring;
    }
    return NULL;
}

char *api_response_text(const cJSON *response)
{
    cJSON *content, *block;
    char  *out = NULL;
    LONG   len = 0;

    content = cJSON_GetObjectItemCaseSensitive((cJSON *)response, "content");
    if (!cJSON_IsArray(content))
        return NULL;

    /* Concatenate every text block's "text" field. */
    cJSON_ArrayForEach(block, content)
    {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(block, "type");
        cJSON *text;

        if (!cJSON_IsString(type) || strcmp(type->valuestring, "text") != 0)
            continue;

        text = cJSON_GetObjectItemCaseSensitive(block, "text");
        if (cJSON_IsString(text))
        {
            LONG tlen = (LONG)strlen(text->valuestring);
            char *grown = realloc(out, len + tlen + 1);
            if (!grown)
            {
                free(out);
                return NULL;
            }
            out = grown;
            memcpy(out + len, text->valuestring, tlen);
            len += tlen;
            out[len] = '\0';
        }
    }

    return out;
}

/* ---- streaming -----------------------------------------------------------
 * Reconstruct the same response object api_create_message returns, but from an
 * SSE stream, printing assistant text live (word-wrapped) as it arrives. */

#define WRAP_COL 78

struct StreamBuilder
{
    cJSON *content;     /* array of reconstructed content blocks            */
    cJSON *cur_block;   /* block currently being assembled (in content)     */
    char  *acc;         /* accumulator: text, or tool input partial_json    */
    LONG   acc_len, acc_cap;
    char   stop_reason[40];
    char   container[80]; /* code-execution container id (from message_start) */
    LONG   col;         /* current output column, for word wrapping         */
};

static void sb_acc_append(struct StreamBuilder *b, const char *p, LONG n)
{
    if (b->acc_len + n + 1 > b->acc_cap)
    {
        LONG ncap = b->acc_cap ? b->acc_cap : 256;
        char *nb;
        while (b->acc_len + n + 1 > ncap) ncap *= 2;
        nb = realloc(b->acc, ncap);
        if (!nb) return;
        b->acc = nb; b->acc_cap = ncap;
    }
    memcpy(b->acc + b->acc_len, p, n);
    b->acc_len += n;
    b->acc[b->acc_len] = '\0';
}

/* Print a text delta live, wrapping at word boundaries near WRAP_COL. */
static void sb_print_wrapped(struct StreamBuilder *b, const char *text)
{
    char  out[2048];
    LONG  o = 0;
    const char *p;

    for (p = text; *p; p++)
    {
        char c = *p;
        if (o > (LONG)sizeof(out) - 2)
        {
            FWrite(Output(), out, o, 1);
            o = 0;
        }
        if (c == '\n')
        {
            out[o++] = '\n';
            b->col = 0;
        }
        else if (c == ' ' && b->col >= WRAP_COL - 6)
        {
            out[o++] = '\n';
            b->col = 0;
        }
        else
        {
            out[o++] = c;
            b->col++;
        }
    }
    if (o > 0)
        FWrite(Output(), out, o, 1);
    Flush(Output());
}

static void sb_event(const char *json, void *ud)
{
    struct StreamBuilder *b = (struct StreamBuilder *)ud;
    cJSON *ev = cJSON_Parse(json);
    cJSON *type;
    const char *t;

    if (!ev)
        return;
    type = cJSON_GetObjectItemCaseSensitive(ev, "type");
    if (!cJSON_IsString(type)) { cJSON_Delete(ev); return; }
    t = type->valuestring;

    if (strcmp(t, "message_start") == 0)
    {
        cJSON *m    = cJSON_GetObjectItemCaseSensitive(ev, "message");
        cJSON *cont = cJSON_GetObjectItemCaseSensitive(m, "container");
        cJSON *id   = cJSON_GetObjectItemCaseSensitive(cont, "id");
        if (cJSON_IsString(id))
        {
            strncpy(b->container, id->valuestring, sizeof(b->container) - 1);
            b->container[sizeof(b->container) - 1] = '\0';
        }
    }
    else if (strcmp(t, "content_block_start") == 0)
    {
        cJSON *cb = cJSON_GetObjectItemCaseSensitive(ev, "content_block");
        b->acc_len = 0;
        if (b->acc) b->acc[0] = '\0';
        if (cb)
        {
            b->cur_block = cJSON_Duplicate(cb, 1);
            cJSON_AddItemToArray(b->content, b->cur_block);
        }
    }
    else if (strcmp(t, "content_block_delta") == 0)
    {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(ev, "delta");
        cJSON *dt    = cJSON_GetObjectItemCaseSensitive(delta, "type");
        if (cJSON_IsString(dt))
        {
            if (strcmp(dt->valuestring, "text_delta") == 0)
            {
                cJSON *txt = cJSON_GetObjectItemCaseSensitive(delta, "text");
                if (cJSON_IsString(txt))
                {
                    sb_print_wrapped(b, txt->valuestring);
                    sb_acc_append(b, txt->valuestring, (LONG)strlen(txt->valuestring));
                }
            }
            else if (strcmp(dt->valuestring, "input_json_delta") == 0)
            {
                cJSON *pj = cJSON_GetObjectItemCaseSensitive(delta, "partial_json");
                if (cJSON_IsString(pj))
                    sb_acc_append(b, pj->valuestring, (LONG)strlen(pj->valuestring));
            }
        }
    }
    else if (strcmp(t, "content_block_stop") == 0)
    {
        if (b->cur_block)
        {
            cJSON *bt = cJSON_GetObjectItemCaseSensitive(b->cur_block, "type");
            if (cJSON_IsString(bt) && strcmp(bt->valuestring, "text") == 0)
            {
                cJSON_DeleteItemFromObjectCaseSensitive(b->cur_block, "text");
                cJSON_AddStringToObject(b->cur_block, "text", b->acc ? b->acc : "");
            }
            else if (b->acc_len > 0)
            {
                cJSON *parsed = cJSON_Parse(b->acc);
                if (parsed)
                {
                    cJSON_DeleteItemFromObjectCaseSensitive(b->cur_block, "input");
                    cJSON_AddItemToObject(b->cur_block, "input", parsed);
                }
            }
        }
        b->cur_block = NULL;
        b->acc_len = 0;
    }
    else if (strcmp(t, "message_delta") == 0)
    {
        cJSON *delta = cJSON_GetObjectItemCaseSensitive(ev, "delta");
        cJSON *sr    = cJSON_GetObjectItemCaseSensitive(delta, "stop_reason");
        if (cJSON_IsString(sr))
        {
            strncpy(b->stop_reason, sr->valuestring, sizeof(b->stop_reason) - 1);
            b->stop_reason[sizeof(b->stop_reason) - 1] = '\0';
        }
    }

    cJSON_Delete(ev);
}

cJSON *api_create_message_streaming(const struct ApiConfig *cfg, cJSON *messages)
{
    char  *body    = NULL;
    char  *headers = NULL;
    char  *errbody = NULL;
    LONG   status  = -1;
    struct StreamBuilder b;
    cJSON *response = NULL;

    if (!cfg || !cfg->api_key || !messages)
        return NULL;

    if (!(body = build_body(cfg, messages, TRUE)))
        return NULL;
    if (!(headers = build_headers(cfg->api_key)))
    {
        free(body);
        return NULL;
    }

    memset(&b, 0, sizeof(b));
    b.content = cJSON_CreateArray();

    if (!https_post_sse(API_HOST, API_PATH, headers,
                        (const UBYTE *)body, (LONG)strlen(body),
                        sb_event, &b, &status, &errbody))
    {
        /* transport/TLS failure */
        cJSON_Delete(b.content);
    }
    else if (status != 200)
    {
        /* Surface the API error object so the caller's error path works. */
        if (errbody)
            response = cJSON_Parse(errbody);
        cJSON_Delete(b.content);
        if (!response)
            Printf("amicode: API request failed (HTTP %ld)\n", status);
    }
    else
    {
        /* Build a response object shaped like the non-streaming one. */
        response = cJSON_CreateObject();
        cJSON_AddStringToObject(response, "type", "message");
        cJSON_AddStringToObject(response, "role", "assistant");
        cJSON_AddItemToObject(response, "content", b.content);
        cJSON_AddStringToObject(response, "stop_reason",
                                b.stop_reason[0] ? b.stop_reason : "end_turn");
        if (b.container[0])
        {
            cJSON *cobj = cJSON_CreateObject();
            cJSON_AddStringToObject(cobj, "id", b.container);
            cJSON_AddItemToObject(response, "container", cobj);
        }
    }

    if (b.acc)   free(b.acc);
    if (errbody) free(errbody);
    free(headers);
    free(body);
    return response;
}
