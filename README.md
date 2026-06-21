# amicode

amicode is a native AmigaOS 3.x Claude agent for m68k Amigas. It brings an
opencode-style assistant into the Amiga Shell: it can chat, keep conversation
history, stream replies as they arrive, inspect files, edit files, run
AmigaDOS commands, search the web, and download files directly on the Amiga.

The project is deliberately not a bridge to a modern machine. The Amiga opens
the TLS connection itself through Roadshow and AmiSSL, talks to Anthropic's
Messages API, and executes tools on the local Amiga filesystem and Shell.

## Status

This is an early native build, but the core shape is in place:

- interactive REPL and single-shot prompts
- streaming assistant output, with `--no-stream` fallback
- native HTTPS transport using `bsdsocket.library` and AmiSSL v5
- Anthropic Messages API support with tool-use turns
- AmigaDOS tools for reading, writing, editing, listing, command execution,
  and downloads
- server-side web search, plus client-side `download_file` for fetching pages
  or archives
- `amicode.config` for local defaults
- optional project context from `AMICODE.md` or `AGENTS.md`

Hardware validation is still ongoing. See `PROGRESS.md` for the detailed build
notes, known issues, and next milestones.

## Target

amicode targets AmigaOS 3.x on m68k. It is developed for an Amiga 1200 class
machine, especially accelerated systems such as PiStorm32-Lite with Emu68.

Runtime requirements:

- AmigaOS 3.x
- Roadshow or another compatible TCP/IP stack exposing `bsdsocket.library`
- AmiSSL v5 runtime, including its certificate store
- enough RAM and stack for AmiSSL and streamed API responses
- an Anthropic API key

Build requirements:

- a host machine capable of cross-compiling for m68k AmigaOS
- vbcc with an AmigaOS 3.x target such as `+a1200`
- Amiga NDK / Roadshow networking headers
- AmiSSL v5 SDK

## Build

The Makefile uses vbcc and lets you provide your own local toolchain paths.
Set these variables to match your machine:

```sh
make \
  VBCC=/path/to/vbcc \
  AMISSL_SDK=/path/to/AmiSSL/Developer \
  NET_INC=/path/to/roadshow/netinclude
```

The build produces an m68k AmigaOS binary named `amicode`.

Clean build output with:

```sh
make clean
```

## Run

Copy the `amicode` binary to the Amiga and run it from the Shell. Roadshow must
be online and AmiSSL v5 must be installed before starting the program.

Set your API key in `ENV:`:

```text
setenv ANTHROPIC_API_KEY sk-ant-...
```

To make it persistent across reboots, also save it to `ENVARC:` using your
usual AmigaOS environment workflow.

For development, amicode also accepts a `.env` file in the current directory:

```text
ANTHROPIC_API_KEY=sk-ant-...
```

Keep API keys out of commits.

Start an interactive chat:

```text
amicode
```

Run a single prompt:

```text
amicode "summarise S:Startup-Sequence"
```

Useful flags:

- `-n`, `--no-stream`: use buffered responses instead of streaming text
- `-v`, `--verbose`: print extra network/TLS diagnostics

## Configuration

If `amicode.config` exists in the current directory, amicode reads simple
`key=value` settings from it:

```text
model=claude-sonnet-4-6
max_tokens=64000
approve=ask
stream=on
```

Supported settings:

- `model`: Anthropic model id to request
- `max_tokens`: response token cap
- `approve`: `ask`, `allow`, or `never`
- `stream`: `on` or `off`

Command-line `--no-stream` takes precedence over `stream=on`.

## Project Context

When launched from a project drawer, amicode looks for `AMICODE.md` first and
then `AGENTS.md`. If either file exists, its contents are added to the system
prompt so the assistant can follow project-specific instructions.

This is useful for coding or maintenance work on the Amiga itself:

```text
Project:
  amicode
  AMICODE.md
  src/
```

Then start `amicode` from `Project:` and ask it to inspect or modify files.

## Tools

The assistant can request tools during a turn. amicode executes them locally
and sends the results back to the model.

Available local tools:

- `read_file`: read a text file from an AmigaDOS path
- `write_file`: create or overwrite a file
- `edit_file`: replace exact text inside an existing file
- `list_dir`: list a directory
- `run_command`: run an AmigaDOS command and capture output
- `download_file`: fetch an HTTP or HTTPS URL to a local path

Actions that write files, run commands, or download files are approval-gated by
default. Set `approve=allow` only in directories and sessions where you are
comfortable with the assistant acting without prompts.

## Architecture

The code is small and direct:

```text
src/
  main.c        Shell entry point, config, API key loading, REPL
  api.c/.h      Anthropic Messages API request, response, and streaming logic
  agent.c/.h    conversation history and tool-use loop
  tools.c/.h    AmigaDOS-backed local tools
  net.c/.h      bsdsocket, AmiSSL, HTTP, HTTPS, chunking, downloads
  json/         vendored cJSON
```

Network traffic goes straight from the Amiga to `api.anthropic.com` over TLS.
Certificate verification is enabled, SNI is set, and HTTP chunked responses are
decoded by the client.

## Development Notes

Some AmigaOS 3.x details are easy to break:

1. Open libraries in this order: `bsdsocket.library`, `amisslmaster.library`,
   then `OpenAmiSSLTagList`. Tear down in reverse.
2. AmiSSL v5 on OS3 uses both `AmiSSLBase` and `AmiSSLExtBase`. One
   `OpenAmiSSLTagList` call fills both; `CloseAmiSSL()` closes both.
3. vbcc cannot compile AmiSSL's GCC stdarg inline macros, so the code uses
   `NO_INLINE_STDARG` and explicit tag arrays.
4. Use `<errno.h>` from vc.lib; do not define your own `errno`.
5. Keep certificate verification on. Do not ship `SSL_VERIFY_NONE`.
6. Keep SNI enabled for Anthropic API requests.
7. The binary requests a large stack via `__stack` because AmiSSL call chains
   are stack-hungry.

The AmiSSL SDK examples, especially its HTTPS example and OS3 autoinit glue,
are useful references when changing the transport layer.

## Releases

Pushing a tag prefixed with `v`, such as `v0.1.0`, runs the GitHub release
workflow. It builds with `OPTFLAGS=-O2`, packages the AmigaOS 3.x m68k binary,
publishes a GitHub Release, and uploads checksums.

The workflow needs access to vbcc, the AmiSSL SDK, and Roadshow netinclude
headers. For a preinstalled runner, configure `RELEASE_RUNNER`, `VBCC`,
`AMISSL_SDK`, and `NET_INC` repository variables. For GitHub-hosted runners,
provide a private toolchain archive through the `AMICODE_TOOLCHAIN_URL` secret.

## Third-Party Code

`src/json/` contains cJSON, which is MIT licensed. Its copyright and license
notice are retained in the vendored source files.

## License

amicode is available under the MIT License. See `LICENSE`.
