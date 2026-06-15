# amicode

A native AmigaOS 3.x agentic Claude client for the Amiga 1200 — an
"opencode for the Amiga." It talks to the Anthropic Messages API over HTTPS,
runs an agentic tool-use loop, and can read/write files and run AmigaDOS
commands on the host machine.

> Status: **Phase 0 complete** (toolchain + AmiSSL/bsdsocket lifecycle).
> See `PROGRESS.md` for the phase plan and current state.

## Target environment

- **Machine:** Amiga 1200 + PiStorm32-Lite running Emu68 (bare-metal 68k JIT).
  There is **no Linux userland** — every agent tool runs Amiga-side via AmigaDOS.
- **OS:** AmigaOS 3.2.x, m68k only (`__AMIGAOS3__`). No OS4/PPC paths.
- **Networking:** Roadshow TCP/IP → `bsdsocket.library`.
- **TLS:** AmiSSL v5 (OpenSSL 3.x-compatible, TLS 1.3, bundled Mozilla CA store).

## Toolchain (as actually installed on this build host)

The original plan named Bebbo `amiga-gcc`, but this machine is set up with
**vbcc** (`m68k-amigaos`), which builds amicode cleanly — including the full
AmiSSL v5 link. We build with vbcc.

| Component | Location (override in `Makefile`) |
|---|---|
| vbcc | `~/opt/vbcc` (uses the `+a1200` config target) |
| NDK 3.2 + Roadshow | `~/opt/vbcc/NDK3.2` |
| AmiSSL v5 SDK | `~/opt/amissl-sdk/AmiSSL/Developer` (v5.27) |

### Build

```sh
make            # produces ./amicode (m68k AmigaOS binary)
make clean
```

Override toolchain paths if yours differ:

```sh
make VBCC=/path/to/vbcc AMISSL_SDK=/path/to/AmiSSL/Developer
```

### Run (on the Amiga)

Copy `amicode` to the A1200 (PCMCIA CF or network) and run from the Shell.
AmiSSL v5 and a running Roadshow stack must be installed.

```
amicode
```

The binary embeds a ~200 KB stack request (`__stack`) for AmiSSL's deep call
chains; if you build a variant that drops it, prefix with `stack 200000` first.

## Architecture

Fully native on the Amiga, no proxy. AmiSSL v5 terminates TLS directly to
`api.anthropic.com`. The agent's tools run Amiga-side because the whole point
is an agent that manipulates *this* Amiga.

```
src/
  main.c        # entry point, banner, (later) REPL + key load
  net.c/.h      # AmiSSL + bsdsocket lifecycle; (later) TLS connect, HTTP, de-chunk
  json/         # cJSON (added in Phase 2)
  ...           # api.c, agent.c, tools.c, ui.c arrive in later phases
```

## Platform landmines (baked into the code — don't regress these)

1. **Library open order:** `bsdsocket` → `amisslmaster` → `OpenAmiSSLTagList`.
   Teardown in reverse. (`net.c`)
2. **AmiSSL v5 spans two bases on OS3** — `AmiSSLBase` *and* `AmiSSLExtBase`.
   Both are filled by one `OpenAmiSSLTagList` call; `CloseAmiSSL()` closes both.
   Never `CloseLibrary()` them directly.
3. **vbcc, not gcc:** AmiSSL's GCC stdarg inline macros (statement-expressions)
   don't compile under vbcc. We `#define NO_INLINE_STDARG` and call the plain
   `...TagList` / `...A` entry points with explicit `TagItem` arrays.
4. **`errno` comes from vc.lib** — do not define your own; just include `<errno.h>`
   and pass `&errno` via `AmiSSL_ErrNoPtr`.
5. **Cert verification ON** (Phase 1): `SSL_CTX_set_default_verify_paths` +
   `SSL_VERIFY_PEER`. Never ship `SSL_VERIFY_NONE`.
6. **SNI is mandatory** (Phase 1): `SSL_set_tlsext_host_name(ssl, host)`.
7. **Seed the RNG** (Phase 1) before any TLS — Amiga entropy is poor.
8. **De-chunk responses** (Phase 1): Cloudflare returns `Transfer-Encoding: chunked`.
9. **Generous stack** (`__stack` in `main.c`) — AmiSSL call chains are stack-hungry.
10. **API key off-disk-in-cleartext** (Phase 2): read `ENV:ANTHROPIC_API_KEY`.

## References

- AmiSSL v5 SDK examples: `~/opt/amissl-sdk/AmiSSL/Developer/Examples/https.c`
  and the OS3 autoinit glue `lib/autoinit_amissl_main.c` (authoritative open
  sequence for vbcc/OS3).
- `sacredbanana/AmigaGPT` `src/openai.c` — shipping LLM-over-AmiSSL client.
- Anthropic Messages API docs (model IDs change — verify at docs.claude.com).
