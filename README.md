# AgentConfig

Tools for working with coding agents such as Claude Code and Codex, built on the
[Malterlib](https://github.com/Malterlib/Malterlib) C++ framework and its build system, `mib`.

| App | What it does |
| --- | --- |
| [ClaudeStatusLine](Apps/ClaudeStatusLine/README.md) | Renders the Claude Code status line: the context window, and burn bars for the weekly limits of Claude and Codex that show whether the quota lasts until the reset at the pace so far. A render takes about 2 ms on Linux and 6–10 ms on macOS; see [Status line overhead](#status-line-overhead). |
| [DesktopHelper](Apps/DesktopHelper/README.md) | A daemon in the desktop session that runs commands for agents connected some other way, such as over SSH, so that they can open apps, show windows and take screenshots. |

## Status line overhead

Claude Code starts the status line command for every render, so its cost is mostly startup. A full render of
ClaudeStatusLine takes about as long as bun starting up and printing `hello`, and a JS status line doing the same
work, reading the status document and the cache, building the bars and printing the line, is two to four times
slower. Median wall time of 1000 runs each, from spawn to exit, in Release builds with the cache fresh:

| Command | Linux, i7-13700K | macOS, M5 Pro | macOS, M1 Max |
| --- | --- | --- | --- |
| `ClaudeStatusLine --status-line` | 1.9–2.0 ms | 5.9–6.0 ms | 9.5–10.2 ms |
| `bun hello.js` | 1.4–1.5 ms | 7.4–7.5 ms | 11.5 ms |
| `bun status.js` | 7.4–7.6 ms | 12.3–12.4 ms | 19.3 ms |
| `node hello.js` | 12.5–12.8 ms | 20.8 ms | 33.0 ms |
| `node status.js` | 12.8–13.1 ms | 21.1 ms | 33.3 ms |
| `/usr/bin/true` | 0.14 ms | 0.57 ms | 1.0 ms |

`/usr/bin/true` is the cost of starting any process on the host. Linux ran bun 1.4.2 and node 24.18 on the
P-cores with the `performance` CPU frequency governor, and the Macs bun 1.3.14 and node 24.22. With the `powersave`
governor a process this short often finishes on an idle core before its clock rises, which made a render, or any
short process, take up to four times as long. The [status line README](Apps/ClaudeStatusLine/README.md#performance)
has the CPU time of a render on each host.

## Getting started

You need Git with Git LFS. Clone the repository and let `mib` bootstrap the rest:

```bash
git clone https://github.com/erikolofsson/AgentConfig.git
cd AgentConfig
./mib setup # macOS only: installs the prerequisites
./mib update-repos
```

The first run of `./mib` fetches Clang, the Malterlib repositories pinned in `Malterlib.MRepo` and the build
tools into the checkout. Nothing is installed outside it, except the `~/.Malterlib` cache.

## Building

Each app has a workspace:

```bash
./mib build AgentConfig                # ClaudeStatusLine
./mib build AgentConfig_DesktopHelper  # DesktopHelper
```

The executables are deployed to `AgentConfig/` in the deploy directory. The READMEs of the apps tell how to install
and use them. `./mib --help` lists the other commands.

## Layout

| Path | Contents |
| --- | --- |
| `Apps/` | The apps, one directory each with its `.MHeader`, sources and README. |
| `AgentConfig.MBuildSystem` | The root build system file: product settings and the repository license configuration. |
| `AgentConfig.MHeader` | The `AgentConfig` workspace. |
| `Malterlib.MRepo` | The pinned commit of `Malterlib/Core`, which pins the other Malterlib modules. |
| `CLAUDE.md`, `AGENTS.md` | Instructions for agents working in the repository. `AGENTS.md` is generated with `./mib update-agents`. |

`Malterlib/`, `External/` and `Binaries/` are sub-repositories that `mib` manages; see the Malterlib documentation for
branches, commits and pushes across them.

## License

Apache License 2.0 with the LLVM Exceptions; see [LICENSE](LICENSE). Files copied from Malterlib keep the copyright of
Unbroken AB under the same license. Every file carries an SPDX header or is covered by [REUSE.toml](REUSE.toml), which
`./mib check-license` checks.
