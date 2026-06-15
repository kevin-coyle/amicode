/* main.c - amicode entry point.
 *
 * amicode: an opencode-style agentic Claude client for the Amiga 1200.
 * Target: AmigaOS 3.2.x / m68k (PiStorm32 + Emu68), vbcc.
 *
 * Phase 3: a conversational REPL. Run bare for an interactive multi-turn chat;
 * pass a prompt on the command line for a single-shot reply.
 *
 *   amicode                       (interactive)
 *   amicode "say hello in one word"   (single-shot)
 */
#include <proto/dos.h>

#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "api.h"
#include "agent.h"
#include "json/cJSON.h"

#define AMICODE_VERSION "0.0.9 (run_command user-shell)"

#define SYSTEM_PROMPT \
    "You are amicode, a helpful agent running natively on an Amiga 1200 " \
    "(AmigaOS 3.2, 68k) with tools to read/write files, run AmigaDOS commands, " \
    "search the web, and download files. To read a specific web page, " \
    "download_file it then read_file it. " \
    "To install software, search the web/Aminet for the package, use " \
    "download_file to fetch its .lha archive (usually to RAM:), then " \
    "run_command with 'lha x' to extract it and copy the program into C: or " \
    "the appropriate drawer. Aminet archives include a .readme describing the " \
    "install; read it when unsure. Prefer m68k/AmigaOS 3.x builds. " \
    "Write and command actions ask the user for approval, so explain what you " \
    "intend before each step. Keep replies concise for an 80-column Shell."

/* AmiSSL/OpenSSL call chains are stack-hungry; size the Shell stack generously
 * so handshakes don't overflow into random crashes. */
unsigned long __stack = 204800;

/* ---- API key loading (ENV: first, then a .env file in the current dir) --- */

static void clean_value(char *s)
{
    LONG n = (LONG)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t'))
        s[--n] = '\0';
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\'')))
    {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static BOOL key_from_envfile(const char *path, char *buf, LONG size)
{
    static const char prefix[] = "ANTHROPIC_API_KEY=";
    char  line[600];
    BPTR  fh;
    BOOL  found = FALSE;

    if (!(fh = Open((STRPTR)path, MODE_OLDFILE)))
        return FALSE;

    while (FGets(fh, (STRPTR)line, sizeof(line)))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, prefix, sizeof(prefix) - 1) == 0)
        {
            strncpy(buf, p + sizeof(prefix) - 1, size - 1);
            buf[size - 1] = '\0';
            clean_value(buf);
            found = (buf[0] != '\0');
            break;
        }
    }

    Close(fh);
    return found;
}

static BOOL load_api_key(char *buf, LONG size)
{
    if (GetVar("ANTHROPIC_API_KEY", buf, size, 0 /* LV_VAR */) > 0)
    {
        clean_value(buf);
        if (buf[0] != '\0')
            return TRUE;
    }
    return key_from_envfile(".env", buf, size);
}

/* ---- helpers ------------------------------------------------------------- */

static BOOL is_verbose_flag(const char *s)
{
    return strcmp(s, "-v") == 0 || strcmp(s, "--verbose") == 0;
}

static BOOL is_nostream_flag(const char *s)
{
    return strcmp(s, "-n") == 0 || strcmp(s, "--no-stream") == 0;
}

static BOOL is_option(const char *s)
{
    return is_verbose_flag(s) || is_nostream_flag(s);
}

/* Join non-flag argv items into a single space-separated prompt (or NULL). */
static char *join_args(int argc, char **argv)
{
    LONG len = 0;
    char *out;
    int i;
    BOOL first = TRUE;

    for (i = 1; i < argc; i++)
        if (!is_option(argv[i]))
            len += (LONG)strlen(argv[i]) + 1;
    if (len == 0)
        return NULL;

    if (!(out = malloc(len + 1)))
        return NULL;
    out[0] = '\0';
    for (i = 1; i < argc; i++)
    {
        if (is_option(argv[i]))
            continue;
        if (!first)
            strcat(out, " ");
        strcat(out, argv[i]);
        first = FALSE;
    }
    return out;
}

/* Send one user turn and print the assistant's reply. Returns FALSE on a hard
 * error worth aborting the session for. */
static BOOL do_turn(struct Conversation *conv, const char *user_text)
{
    const char *err = NULL;
    char *text;

    if (!conv_add_user_text(conv, user_text))
    {
        Printf("amicode: out of memory\n");
        return FALSE;
    }

    text = conv_run_turn(conv, &err);
    if (err)
    {
        Printf("API error: %s\n", (LONG)err);
        return TRUE;   /* recoverable: stay in the REPL */
    }

    if (conv->stream)
    {
        /* Reply was already printed live during streaming; nothing to echo. */
        if (text)
            free(text);
    }
    else if (text)
    {
        Printf("%s\n", (LONG)text);
        free(text);
    }
    else
    {
        Printf("amicode: (no text reply)\n");
    }
    return TRUE;
}

static BOOL is_quit(const char *s)
{
    return strcmp(s, "quit") == 0 || strcmp(s, "exit") == 0 ||
           strcmp(s, "bye")  == 0;
}

/* ---- interactive REPL ---------------------------------------------------- */

static void repl(struct Conversation *conv)
{
    char line[2048];
    BPTR in  = Input();
    BPTR out = Output();

    Printf("Interactive chat. Type 'quit' to exit.\n");

    for (;;)
    {
        LONG n;

        FPuts(out, (STRPTR)"\n> ");
        Flush(out);

        if (!FGets(in, (STRPTR)line, sizeof(line)))
            break;                      /* EOF (Ctrl-\) */

        n = (LONG)strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';

        if (n == 0)
            continue;
        if (is_quit(line))
            break;

        if (!do_turn(conv, line))
            break;
    }
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char key[256];
    struct Conversation conv;
    int rc = 0;
    int i;
    BOOL streaming = TRUE;

    Printf("amicode %s\n", (LONG)AMICODE_VERSION);

    for (i = 1; i < argc; i++)
    {
        if (is_verbose_flag(argv[i]))  net_verbose = TRUE;
        if (is_nostream_flag(argv[i])) streaming = FALSE;
    }

    if (!load_api_key(key, sizeof(key)))
    {
        Printf("amicode: no API key found.\n"
               "  Either set ENV:  setenv ANTHROPIC_API_KEY sk-ant-...\n"
               "  or put a .env file (ANTHROPIC_API_KEY=sk-ant-...) in the\n"
               "  current directory.\n");
        return 20;
    }

    if (!net_init())
    {
        Printf("amicode: network init failed; aborting.\n");
        return 20;
    }

    if (!conv_init(&conv, key, SYSTEM_PROMPT))
    {
        Printf("amicode: out of memory\n");
        net_cleanup();
        return 20;
    }
    conv.stream = streaming;

    {
        /* A non-flag prompt on the command line -> single-shot; else REPL. */
        char *prompt = join_args(argc, argv);
        if (prompt)
        {
            do_turn(&conv, prompt);
            free(prompt);
        }
        else
        {
            repl(&conv);
        }
    }

    conv_free(&conv);
    net_cleanup();
    return rc;
}
