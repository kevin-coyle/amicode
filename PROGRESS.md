# amicode — build progress

## Phase 0 — Toolchain + clean skeleton  ✅ DONE (validated on real A1200)

> Confirmed on hardware: "clean teardown". Required installing the matching
> AmiSSL **5.27** runtime on the Amiga — the binary requests API version
> `AMISSL_V362` and amisslmaster is not forward-compatible, so an older
> runtime fails `OpenAmiSSLTagList` with code 1.


- [x] vbcc `+a1200` cross-build produces an m68k AmigaOS binary.
- [x] AmiSSL v5.27 SDK downloaded + wired into the include path.
- [x] `net.c` brings up `bsdsocket` → `amisslmaster` → `OpenAmiSSLTagList`
      (fills both `AmiSSLBase` + `AmiSSLExtBase`) and tears down in reverse.
- [x] `main.c` prints a banner, runs init/teardown, sets a ~200 KB `__stack`.
- [x] **Full AmiSSL + bsdsocket lifecycle compiles AND links under vbcc** —
      the key toolchain risk. Binary runs under `vamos`; banner + DOS output OK.

**Remaining acceptance (hardware):** run on the real Emu68 A1200 and confirm
"OK: clean teardown, no library bases leaked." `vamos` can't emulate
`bsdsocket.library`, so the library open/close path is only verifiable on the
Amiga.

### Key decisions / deviations from the original plan
- **Toolchain is vbcc, not Bebbo amiga-gcc** (vbcc is what's installed; it
  builds everything including AmiSSL). See README "Platform landmines" 3–4 for
  the vbcc-specific AmiSSL handling.
- **Open sequence uses `OpenAmiSSLTagList` (master)**, not the older
  `OpenAmiSSLTags`+`InitAmiSSL` split from the spec's §6 snippet — this matches
  the current AmiSSL 5.27 SDK autoinit glue and fills both OS3 library bases.

## Phase 1 — Verified TLS pipe to Anthropic  🔨 built; awaiting A1200 test
`net.c` now: seeds RNG, builds a shared `SSL_VERIFY_PEER` `SSL_CTX`
(`set_default_verify_paths` → AmiSSL CA store), and `https_post()` does
TCP connect → SNI → verified handshake (prints TLS version + cipher) →
HTTP/1.1 POST → read-until-close → de-chunk → parse status + body.
`main.c` sends a keyless POST to expose a 401.

**Awaiting hardware:** confirm handshake verifies, cipher prints, and a 401
JSON body comes back de-chunked. Needs the AmiSSL 5.27 installer to have run
(CA certs + `AmiSSL:` assign) for `set_default_verify_paths` to find the store.

## Phase 2 — One non-streaming round-trip  🔨 built; awaiting A1200 test
- cJSON 1.7.18 vendored in `src/json/` (compiles clean under vbcc; needs
  `-lmieee` for its `double` math — `strtod`/`fabs`/MathIeeeDoubBas).
- `api.c`/`api.h`: build the Messages request from a cJSON message array
  (`api_create_message`), extract assistant text (`api_response_text`), surface
  API errors (`api_error_message`). Returns the raw parsed response so Phase 4
  can read `tool_use` blocks + `stop_reason`.
- `main.c`: loads the key from `ENV:ANTHROPIC_API_KEY`, **falling back to a
  `.env` file (`ANTHROPIC_API_KEY=...`) in the current dir**; sends the
  command-line prompt; prints the reply.

**Acceptance (hardware):** `amicode "say hello in one word"` prints a one-word
reply. Run from the drawer that contains `.env` (or set the ENV var).

## Phase 3 — Conversation REPL  🔨 built; awaiting A1200 test
- `agent.c`/`agent.h`: `struct Conversation` owns the cJSON message history;
  `conv_run_turn` sends the full history and appends the assistant's **whole
  content array** (forward-compatible with Phase 4 tool_use blocks).
- `main.c`: bare `amicode` → interactive REPL (reads Shell lines via `FGets`,
  `quit`/`exit`/`bye` or Ctrl-\ to leave); `amicode "prompt"` → single-shot.

**Acceptance (hardware):** coherent multi-turn chat — e.g. tell it your name,
then ask what it is, and confirm it remembers across turns.

## Phase 4 — The agent (tool loop)  🔨 built; awaiting A1200 test
- `tools.c`/`tools.h`: four AmigaDOS tools — `read_file` (Open/Read, 64KB cap),
  `write_file` (MODE_NEWFILE/Write, approval-gated), `list_dir`
  (Lock/Examine/ExNext), `run_command` (SystemTags → temp file → read back,
  approval-gated). `tools_definitions()` emits the Anthropic tools schema.
- `agent.c`: `conv_run_turn` is now the tool-use loop — sends history, on
  `stop_reason == "tool_use"` prints narration, runs each tool (`-> name`),
  appends `tool_result` blocks, and re-sends until `end_turn`
  (MAX_TOOL_ROUNDS = 16).
- Approval gate: write_file / run_command print a `[approve] ... (y/N)` prompt
  read from the console before acting.

**Acceptance (hardware):**
1. "List RAM: and tell me the largest file" → list_dir → reasoned answer.
2. "Create RAM:hello.txt containing HELLO" → approval → file exists, correct.
3. A two-step task (read a file, then write a modified copy) across rounds.

### Phase 4+ extensions (built; awaiting A1200 test)
- **Web access (native, no second key):** Anthropic server-side `web_search`
  tool. NOTE: switched from `web_search_20260209`/`web_fetch_20260209` to the
  plain **`web_search_20250305`** — the newer tools use server-side code
  execution + a container that must be threaded through every follow-up
  request, which kept failing with "container_id is required when there are
  pending tool uses". The 20250305 tool has no code execution and no container.
  `web_fetch` dropped (only a code-exec version exists); to read a page the
  model uses client-side download_file + read_file. `agent.c` still handles
  `pause_turn` and prints `server_tool_use` activity; the container-id capture
  (api.c/agent.c) remains in place, harmless, for any future code-exec tool.
- **download_file tool:** `net.c` `http_download()` — http/https GET with a
  TLS/plain `Conn` abstraction, URL parsing, redirect following, de-chunking,
  binary-safe write to disk. Approval-gated. Enables "install X" workflows:
  web_search the Aminet page → download_file the .lha to RAM: → run_command
  "lha x" to extract/install. System prompt updated to teach this flow.

## ⚠️ TO REVISIT — download_file possible hang
On first hardware test, "install vim" download was ambiguous (seemed to hang).
Most likely cause: **no progress output** during a multi-MB transfer (we print
"downloading..." then nothing until done → looks hung). Also consider: no
socket read timeout (a stalled connection blocks forever), and very large
bodies held wholly in RAM. When revisiting: add byte-progress output, a
read timeout via SO_RCVTIMEO / SocketBaseTags, and stream-to-file instead of
buffer-then-write. Streaming (Phase 5) should make working transfers visibly
progress.

## Phase 5 — Streaming + console polish  🔨 built; awaiting A1200 test
- `net.c` `https_post_sse()`: streaming SSE transport — reads the
  `text/event-stream` response, **de-chunks on the fly** (chunk-decoder state
  machine) and assembles `data:` lines, invoking a callback per event. Non-2xx
  bodies returned whole for error parsing.
- `api.c` `api_create_message_streaming()`: sends `"stream":true`, prints
  assistant `text_delta` **live, word-wrapped to ~78 cols**, and reconstructs
  the same response object (text + tool_use via `input_json_delta`, server-tool
  blocks, `stop_reason`) — so the Phase-4 tool loop is unchanged.
- `agent.c`: `conv_run_turn` picks streaming vs non-streaming via `conv.stream`;
  text already-streamed isn't re-printed; server-tool activity still shown.
- `main.c`: streaming **on by default**; `-n` / `--no-stream` falls back to the
  buffered path. Version 0.0.5.

**Acceptance (hardware):** tokens appear live as Claude replies; tool calls and
web_search still work under streaming; output readable on an 80-col Shell.
`-n` gives the old buffered behaviour.
## Phase 6 — Optional niceties (no GUI)  🔨 in progress
Batch A built (v0.1.0):
- **Config file** `amicode.config` (current dir): `model`, `max_tokens`,
  `approve` (ask|allow|never), `stream` (on|off). Parsed in main.c.
- **Approval policy**: `tools_approval_mode` (APPROVE_ASK/ALLOW/DENY) in
  tools.c; `approve()` honours it. `-n` flag still overrides streaming.
- **Project context**: reads `AMICODE.md` / `AGENTS.md` from the current dir
  into the system prompt (`build_system_prompt`, 32 KB cap).
- **Download progress**: `conn_read_all` prints "received N KB" so a working
  transfer no longer looks hung. (A true read timeout needs WaitSelect —
  Roadshow headers lack SO_RCVTIMEO/timeval — deferred; revisit if it actually
  stalls vs just-slow.)
- `to_utf8` moved to `api.c` as `api_to_utf8` (exposed) and now also UTF-8s the
  system prompt (so AMICODE.md with Latin-1 bytes is safe).

### web_search pairing fix (v0.1.1)
"server_tool_use ... without a corresponding web_search_tool_result block",
intermittent + unrecoverable. Cause: on `pause_turn`, the paused assistant turn
(holding the server_tool_use) and the continuation (holding the result) were
stored as TWO assistant messages, breaking the required same-message pairing.
Fix in `agent.c`: `conv_run_turn` now keeps one in-progress assistant turn and
**merges** pause_turn continuations into it (`begin_assistant_turn` /
`merge_into_assistant_turn`), and `sanitize_server_tools` drops any orphan
server_tool_use at turn finalization (safety net for truncated streams) so a
broken turn can't poison the whole session.

Batch B (next): **ARexx port** — public message port "AMICODE", run prompts via
ARexx and return RESULT.
