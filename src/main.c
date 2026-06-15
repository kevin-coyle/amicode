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
#include <stdio.h>

#include "net.h"
#include "api.h"
#include "agent.h"
#include "tools.h"
#include "json/cJSON.h"

#define AMICODE_VERSION "0.1.5 (capture stderr + fault)"

#define CONFIG_FILE  "amicode.config"
#define PROJECT_FILE "AMICODE.md"
#define PROJECT_ALT  "AGENTS.md"

struct Config
{
    char model[64];     /* "" = leave default      */
    LONG max_tokens;    /* 0  = leave default       */
    int  approve;       /* -1 unset, else APPROVE_* */
    int  stream;        /* -1 unset, else 0/1       */
};

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
    "intend before each step. To change part of an existing file use edit_file " \
    "(exact-string replace) rather than rewriting it; use write_file for new " \
    "files or full rewrites. Do not split large writes into temp files and " \
    "concatenate. Keep replies concise for an 80-column Shell."

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

/* ---- config file + project context --------------------------------------- */

/* Parse amicode.config (key=value lines, # comments) from the current dir.
 * Unset keys keep their sentinel so defaults survive. */
static void load_config(struct Config *c)
{
    char line[256];
    BPTR fh;

    c->model[0]   = '\0';
    c->max_tokens = 0;
    c->approve    = -1;
    c->stream     = -1;

    if (!(fh = Open((STRPTR)CONFIG_FILE, MODE_OLDFILE)))
        return;

    while (FGets(fh, (STRPTR)line, sizeof(line)))
    {
        char *p = line, *eq, *key, *val;
        LONG kl;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
            continue;
        if (!(eq = strchr(p, '=')))
            continue;

        *eq = '\0';
        key = p;
        val = eq + 1;
        clean_value(val);                 /* trims ws/quotes/newline */
        kl = (LONG)strlen(key);
        while (kl > 0 && (key[kl - 1] == ' ' || key[kl - 1] == '\t'))
            key[--kl] = '\0';

        if (strcmp(key, "model") == 0)
        {
            strncpy(c->model, val, sizeof(c->model) - 1);
            c->model[sizeof(c->model) - 1] = '\0';
        }
        else if (strcmp(key, "max_tokens") == 0)
        {
            c->max_tokens = atol(val);
        }
        else if (strcmp(key, "approve") == 0)
        {
            if (strcmp(val, "allow") == 0 || strcmp(val, "always") == 0)
                c->approve = APPROVE_ALLOW;
            else if (strcmp(val, "never") == 0 || strcmp(val, "deny") == 0)
                c->approve = APPROVE_DENY;
            else
                c->approve = APPROVE_ASK;
        }
        else if (strcmp(key, "stream") == 0)
        {
            c->stream = (strcmp(val, "off") == 0 || strcmp(val, "no") == 0 ||
                         strcmp(val, "false") == 0) ? 0 : 1;
        }
    }

    Close(fh);
}

/* Read a text file (capped at 32 KB) into a malloc'd, NUL-terminated buffer. */
static char *read_text_file(const char *path)
{
    BPTR  fh;
    char *buf;
    LONG  cap = 4096, len = 0;

    if (!(fh = Open((STRPTR)path, MODE_OLDFILE)))
        return NULL;
    if (!(buf = malloc(cap))) { Close(fh); return NULL; }

    for (;;)
    {
        LONG n;
        if (len + 1024 + 1 > cap)
        {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); Close(fh); return NULL; }
            buf = nb; cap *= 2;
        }
        n = Read(fh, buf + len, 1024);
        if (n <= 0)
            break;
        len += n;
        if (len >= 32768)
            break;
    }
    Close(fh);
    buf[len] = '\0';
    return buf;
}

/* Build the system prompt: base prompt plus AMICODE.md / AGENTS.md project
 * context from the current dir, if present. Returns a malloc'd string the
 * caller must free, or NULL to mean "use the base prompt as-is". */
static char *build_system_prompt(void)
{
    char *proj = read_text_file(PROJECT_FILE);
    char *sysprompt;
    LONG  need;

    if (!proj)
        proj = read_text_file(PROJECT_ALT);
    if (!proj)
        return NULL;

    need = (LONG)strlen(SYSTEM_PROMPT) + (LONG)strlen(proj) + 64;
    sysprompt = malloc(need);
    if (sysprompt)
        sprintf(sysprompt, "%s\n\n--- Project context (%s) ---\n%s",
                SYSTEM_PROMPT, PROJECT_FILE, proj);
    free(proj);
    return sysprompt;
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
    struct Config config;
    char *sysprompt;
    int rc = 0;
    int i;
    BOOL nostream_flag = FALSE;

    Printf("amicode %s\n", (LONG)AMICODE_VERSION);

    for (i = 1; i < argc; i++)
    {
        if (is_verbose_flag(argv[i]))  net_verbose = TRUE;
        if (is_nostream_flag(argv[i])) nostream_flag = TRUE;
    }

    if (!load_api_key(key, sizeof(key)))
    {
        Printf("amicode: no API key found.\n"
               "  Either set ENV:  setenv ANTHROPIC_API_KEY sk-ant-...\n"
               "  or put a .env file (ANTHROPIC_API_KEY=sk-ant-...) in the\n"
               "  current directory.\n");
        return 20;
    }

    load_config(&config);
    sysprompt = build_system_prompt();   /* AMICODE.md/AGENTS.md, or NULL */
    if (sysprompt)
        Printf("(loaded project context from %s)\n", (LONG)PROJECT_FILE);

    if (!net_init())
    {
        Printf("amicode: network init failed; aborting.\n");
        if (sysprompt) free(sysprompt);
        return 20;
    }

    if (!conv_init(&conv, key, sysprompt ? sysprompt : SYSTEM_PROMPT))
    {
        Printf("amicode: out of memory\n");
        net_cleanup();
        if (sysprompt) free(sysprompt);
        return 20;
    }

    /* Apply config: command-line -n always wins for streaming. */
    if (config.model[0])      conv.cfg.model      = config.model;
    if (config.max_tokens > 0) conv.cfg.max_tokens = config.max_tokens;
    if (config.approve != -1)  tools_approval_mode = config.approve;
    conv.stream = nostream_flag ? FALSE
                : (config.stream != -1) ? (BOOL)config.stream : TRUE;

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
    if (sysprompt) free(sysprompt);
    return rc;
}
