/* tools.c - agent tools for amicode, implemented with AmigaDOS. */

#include <proto/exec.h>
#include <proto/dos.h>

#include <dos/dos.h>
#include <dos/dostags.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "tools.h"
#include "net.h"

/* Bound tool output so a huge file can't blow up token usage or RAM. */
#define MAX_READ_BYTES   65536L
#define CMD_TMP_FILE     "T:amicode-cmd.out"

/* ---- small growable string buffer --------------------------------------- */

struct sbuf { char *data; LONG len, cap; };

static BOOL sb_init(struct sbuf *s)
{
    s->cap = 1024;
    s->len = 0;
    s->data = malloc(s->cap);
    if (s->data) s->data[0] = '\0';
    return s->data != NULL;
}

static BOOL sb_append(struct sbuf *s, const char *p, LONG n)
{
    if (s->len + n + 1 > s->cap)
    {
        LONG ncap = s->cap;
        char *nd;
        while (s->len + n + 1 > ncap) ncap *= 2;
        nd = realloc(s->data, ncap);
        if (!nd) return FALSE;
        s->data = nd;
        s->cap  = ncap;
    }
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
    return TRUE;
}

static BOOL sb_puts(struct sbuf *s, const char *p) { return sb_append(s, p, (LONG)strlen(p)); }

/* Return a malloc'd copy of a plain message (for simple results/errors). */
static char *dup_msg(const char *msg)
{
    char *r = malloc(strlen(msg) + 1);
    if (r) strcpy(r, msg);
    return r;
}

/* ---- approval gate ------------------------------------------------------- */

static BOOL approve(const char *summary)
{
    char line[64];

    Printf("\n  [approve] %s\n  Proceed? (y/N) ", (LONG)summary);
    Flush(Output());

    if (!FGets(Input(), (STRPTR)line, sizeof(line)))
        return FALSE;
    return line[0] == 'y' || line[0] == 'Y';
}

/* ---- shared: read a whole file into a malloc'd buffer -------------------- */

static char *read_whole_file(const char *path, LONG *out_len, const char **err)
{
    BPTR  fh;
    char *buf;
    LONG  cap = 8192, len = 0;

    *err = NULL;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE)))
    {
        *err = "couldn't open file";
        return NULL;
    }

    if (!(buf = malloc(cap)))
    {
        Close(fh);
        *err = "out of memory";
        return NULL;
    }

    for (;;)
    {
        LONG n;
        if (len + 4096 + 1 > cap)
        {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); Close(fh); *err = "out of memory"; return NULL; }
            buf = nb; cap *= 2;
        }
        n = Read(fh, buf + len, 4096);
        if (n <= 0)
            break;
        len += n;
        if (len >= MAX_READ_BYTES) break;   /* caller re-checks size */
    }
    Close(fh);

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* ---- read_file ----------------------------------------------------------- */

static char *tool_read_file(const cJSON *input, BOOL *is_error)
{
    cJSON *p = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "path");
    const char *err;
    char *content;
    LONG len;

    if (!cJSON_IsString(p)) { *is_error = TRUE; return dup_msg("read_file: missing 'path'"); }

    content = read_whole_file(p->valuestring, &len, &err);
    if (!content)
    {
        struct sbuf s;
        *is_error = TRUE;
        if (!sb_init(&s)) return dup_msg("out of memory");
        sb_puts(&s, "read_file failed: ");
        sb_puts(&s, err ? err : "unknown error");
        return s.data;
    }

    if (len >= MAX_READ_BYTES)
    {
        /* Append a truncation note (content already NUL-terminated). */
        struct sbuf s;
        if (sb_init(&s))
        {
            sb_append(&s, content, len);
            sb_puts(&s, "\n\n[amicode: file truncated at 64KB]");
            free(content);
            return s.data;
        }
    }
    return content;
}

/* ---- write_file ---------------------------------------------------------- */

static char *tool_write_file(const cJSON *input, BOOL *is_error)
{
    cJSON *p = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "path");
    cJSON *c = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "content");
    const char *path, *content;
    LONG clen, wrote;
    BPTR fh;
    char summary[256];
    char result[256];

    if (!cJSON_IsString(p) || !cJSON_IsString(c))
    { *is_error = TRUE; return dup_msg("write_file: needs 'path' and 'content'"); }

    path    = p->valuestring;
    content = c->valuestring;
    clen    = (LONG)strlen(content);

    sprintf(summary, "write_file: %ld bytes -> %s", (long)clen, path);
    if (!approve(summary))
    { *is_error = TRUE; return dup_msg("write declined by user"); }

    if (!(fh = Open((STRPTR)path, MODE_NEWFILE)))
    { *is_error = TRUE; return dup_msg("write_file: couldn't open file for writing"); }

    wrote = Write(fh, (APTR)content, clen);
    Close(fh);

    if (wrote != clen)
    { *is_error = TRUE; return dup_msg("write_file: short write (disk full?)"); }

    sprintf(result, "Wrote %ld bytes to %s", (long)wrote, path);
    return dup_msg(result);
}

/* ---- list_dir ------------------------------------------------------------ */

static char *tool_list_dir(const cJSON *input, BOOL *is_error)
{
    cJSON *p = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "path");
    const char *path;
    BPTR lock;
    struct FileInfoBlock *fib;
    struct sbuf s;
    char line[320];
    int count = 0;

    if (!cJSON_IsString(p)) { *is_error = TRUE; return dup_msg("list_dir: missing 'path'"); }
    path = p->valuestring;

    if (!(lock = Lock((STRPTR)path, ACCESS_READ)))
    { *is_error = TRUE; return dup_msg("list_dir: couldn't lock path"); }

    if (!(fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL)))
    { UnLock(lock); *is_error = TRUE; return dup_msg("out of memory"); }

    if (!Examine(lock, fib))
    { FreeDosObject(DOS_FIB, fib); UnLock(lock); *is_error = TRUE;
      return dup_msg("list_dir: Examine failed"); }

    if (fib->fib_DirEntryType < 0)
    { FreeDosObject(DOS_FIB, fib); UnLock(lock); *is_error = TRUE;
      return dup_msg("list_dir: path is a file, not a directory"); }

    if (!sb_init(&s))
    { FreeDosObject(DOS_FIB, fib); UnLock(lock); *is_error = TRUE;
      return dup_msg("out of memory"); }

    sb_puts(&s, "Contents of ");
    sb_puts(&s, path);
    sb_puts(&s, ":\n");

    while (ExNext(lock, fib))
    {
        if (fib->fib_DirEntryType > 0)
            sprintf(line, "  %-30s <dir>\n", fib->fib_FileName);
        else
            sprintf(line, "  %-30s %ld bytes\n",
                    fib->fib_FileName, (long)fib->fib_Size);
        sb_puts(&s, line);
        count++;
    }

    if (count == 0)
        sb_puts(&s, "  (empty)\n");

    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return s.data;
}

/* ---- run_command --------------------------------------------------------- */

static char *tool_run_command(const cJSON *input, BOOL *is_error)
{
    cJSON *c = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "command");
    const char *cmd;
    char summary[256];
    BPTR out, in;
    LONG rc;
    char *output;
    const char *err;
    LONG olen;
    struct sbuf s;
    char hdr[64];

    if (!cJSON_IsString(c)) { *is_error = TRUE; return dup_msg("run_command: missing 'command'"); }
    cmd = c->valuestring;

    sprintf(summary, "run_command: %.200s", cmd);
    if (!approve(summary))
    { *is_error = TRUE; return dup_msg("command declined by user"); }

    if (!(out = Open((STRPTR)CMD_TMP_FILE, MODE_NEWFILE)))
    { *is_error = TRUE; return dup_msg("run_command: couldn't open temp output file"); }
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);

    /* Synchronous System(): it does NOT close the streams we pass, so we do.
     * SYS_UserShell runs the command through the user's real Shell so streams
     * are inherited properly, redirection works, and script/alias handling
     * matches an interactive Shell — without it some commands run detached or
     * in their own console and their output can't be captured here. */
    rc = SystemTags((STRPTR)cmd,
                    SYS_Input,     (ULONG)in,
                    SYS_Output,    (ULONG)out,
                    SYS_UserShell, TRUE,
                    SYS_Asynch,    FALSE,
                    TAG_DONE);

    Close(out);
    if (in) Close(in);

    if (rc == -1)
    {
        DeleteFile((STRPTR)CMD_TMP_FILE);
        *is_error = TRUE;
        return dup_msg("run_command: command could not be executed");
    }

    output = read_whole_file(CMD_TMP_FILE, &olen, &err);
    DeleteFile((STRPTR)CMD_TMP_FILE);

    if (!sb_init(&s))
    { free(output); *is_error = TRUE; return dup_msg("out of memory"); }

    sprintf(hdr, "exit code %ld\n", (long)rc);
    sb_puts(&s, hdr);
    if (output && olen > 0)
    {
        sb_append(&s, output, olen);
    }
    else
    {
        /* No captured output: either the command genuinely printed nothing, or
         * it detached / opened its own window. Tell the model so it can adapt
         * (e.g. append '>RAM:out' redirection and read_file the result). */
        sb_puts(&s, "(no output captured. If you expected output, the command "
                    "may have run detached or in its own window. Try appending "
                    "output redirection like '>RAM:cmd.out' to the command, "
                    "then read_file RAM:cmd.out.)");
    }
    if (output) free(output);

    return s.data;
}

/* ---- download_file ------------------------------------------------------- */

static char *tool_download_file(const cJSON *input, BOOL *is_error)
{
    cJSON *u = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "url");
    cJSON *p = cJSON_GetObjectItemCaseSensitive((cJSON *)input, "path");
    const char *url, *path;
    char summary[400], result[256];
    LONG status = -1, len = 0;

    if (!cJSON_IsString(u) || !cJSON_IsString(p))
    { *is_error = TRUE; return dup_msg("download_file: needs 'url' and 'path'"); }
    url  = u->valuestring;
    path = p->valuestring;

    sprintf(summary, "download_file: %.200s -> %s", url, path);
    if (!approve(summary))
    { *is_error = TRUE; return dup_msg("download declined by user"); }

    Printf("  downloading...\n");
    if (!http_download(url, path, &status, &len))
    {
        *is_error = TRUE;
        sprintf(result, "download_file failed (HTTP %ld)", (long)status);
        return dup_msg(result);
    }

    sprintf(result, "Downloaded %ld bytes to %s (HTTP %ld)",
            (long)len, path, (long)status);
    return dup_msg(result);
}

/* ---- dispatch ------------------------------------------------------------ */

char *tools_execute(const char *name, const cJSON *input, BOOL *is_error)
{
    *is_error = FALSE;

    if (strcmp(name, "read_file")     == 0) return tool_read_file(input, is_error);
    if (strcmp(name, "write_file")    == 0) return tool_write_file(input, is_error);
    if (strcmp(name, "list_dir")      == 0) return tool_list_dir(input, is_error);
    if (strcmp(name, "run_command")   == 0) return tool_run_command(input, is_error);
    if (strcmp(name, "download_file") == 0) return tool_download_file(input, is_error);

    *is_error = TRUE;
    return dup_msg("unknown tool");
}

/* ---- definitions --------------------------------------------------------- */

/* Helper: add one {name, description, input_schema{properties, required}}. */
static void add_tool(cJSON *arr, const char *name, const char *desc,
                     const char *p1, const char *p1desc,
                     const char *p2, const char *p2desc)
{
    cJSON *tool   = cJSON_CreateObject();
    cJSON *schema = cJSON_CreateObject();
    cJSON *props  = cJSON_CreateObject();
    cJSON *req    = cJSON_CreateArray();
    cJSON *prop1  = cJSON_CreateObject();

    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", desc);

    cJSON_AddStringToObject(prop1, "type", "string");
    cJSON_AddStringToObject(prop1, "description", p1desc);
    cJSON_AddItemToObject(props, p1, prop1);
    cJSON_AddItemToArray(req, cJSON_CreateString(p1));

    if (p2)
    {
        cJSON *prop2 = cJSON_CreateObject();
        cJSON_AddStringToObject(prop2, "type", "string");
        cJSON_AddStringToObject(prop2, "description", p2desc);
        cJSON_AddItemToObject(props, p2, prop2);
        cJSON_AddItemToArray(req, cJSON_CreateString(p2));
    }

    cJSON_AddStringToObject(schema, "type", "object");
    cJSON_AddItemToObject(schema, "properties", props);
    cJSON_AddItemToObject(schema, "required", req);
    cJSON_AddItemToObject(tool, "input_schema", schema);

    cJSON_AddItemToArray(arr, tool);
}

cJSON *tools_definitions(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return NULL;

    add_tool(arr, "read_file",
        "Read a file on the Amiga and return its contents as text.",
        "path", "AmigaDOS path, e.g. RAM:notes.txt or S:Startup-Sequence",
        NULL, NULL);

    add_tool(arr, "write_file",
        "Create or overwrite a file with text content. The user is asked to "
        "approve before anything is written.",
        "path", "AmigaDOS path to write, e.g. RAM:hello.txt",
        "content", "The full text to write to the file");

    add_tool(arr, "list_dir",
        "List the entries (name, size, dir/file) in a directory.",
        "path", "Directory path, e.g. RAM: or SYS: or Work:",
        NULL, NULL);

    add_tool(arr, "run_command",
        "Run an AmigaDOS Shell command and return its output. The user is "
        "asked to approve before the command runs.",
        "command", "The command line to run, e.g. \"list RAM:\" or \"version\"",
        NULL, NULL);

    add_tool(arr, "download_file",
        "Download a file from an http:// or https:// URL (e.g. an Aminet .lha "
        "archive) and save it to a local AmigaDOS path. Follows redirects and "
        "is binary-safe. Asks the user to approve first. To install software, "
        "typically: web_search for the Aminet page, download_file the archive "
        "to RAM:, then run_command \"lha x RAM:foo.lha RAM:\" to extract and "
        "copy the result into C: or the right drawer.",
        "url", "Full http/https URL to download",
        "path", "Local AmigaDOS path to save to, e.g. RAM:vim.lha");

    /* Anthropic server-side web search: declared by type+name only, no schema
     * and no client handler — Claude runs it on Anthropic's servers and returns
     * results inline; we carry the result blocks through history.
     *
     * We deliberately use the 20250305 tool, NOT web_search_20260209: the newer
     * version performs "dynamic filtering" via server-side code execution,
     * which requires threading a container id through every follow-up request.
     * The 20250305 tool is plain server-side search with no code execution and
     * no container — far simpler and more robust here. To read a specific page,
     * the model uses the client-side download_file + read_file tools instead. */
    {
        cJSON *ws = cJSON_CreateObject();
        cJSON_AddStringToObject(ws, "type", "web_search_20250305");
        cJSON_AddStringToObject(ws, "name", "web_search");
        cJSON_AddItemToArray(arr, ws);
    }

    return arr;
}
