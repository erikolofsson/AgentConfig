# Claude Code Status Line

A Malterlib distributed app tool that renders the Claude Code status line.

Claude Code runs the configured `statusLine` command once per render, writes a JSON status document to
its stdin, and displays the first line the command writes to stdout.

## Building

```bash
MalterlibBuildShowProgress=false ./mib build AgentConfig
```

The executable is deployed to `/opt/Deploy/AgentConfig/ClaudeStatusLine` on macOS
(`/Deploy/...` on Linux, `/c/Deploy/...` on Windows).

## Using it from Claude Code

```bash
/opt/Deploy/AgentConfig/ClaudeStatusLine --install
```

This sets `statusLine` in `~/.claude/settings.json` to run the executable with `--status-line`,
keeping every other setting as it is (`--settings` picks another file). The equivalent by hand:

```json
{
	"statusLine": {
		"type": "command",
		"command": "/opt/Deploy/AgentConfig/ClaudeStatusLine --status-line",
		"refreshInterval": 10
	}
}
```

Claude Code's own triggers are conversation events, so `refreshInterval` is what keeps the burn bars
and the times to reset moving while a session sits idle. `--refresh-interval` changes the seconds,
and zero leaves the setting out.

The command is written for the shell Claude Code runs it with. That is Bash, except on Windows without Git Bash, where
it is PowerShell, which needs `& '...'` to run a quoted path. `--install` finds Git Bash the way Claude Code does, from
`CLAUDE_CODE_GIT_BASH_PATH`, the default Git directories and the git on the PATH. When Claude Code starts with another
environment than the terminal `--install` runs in, `--shell bash` or `--shell powershell` names the shell instead.

## Trying it out without Claude Code

`--input` takes the status document directly, so the layout can be iterated on without piping:

```bash
/opt/Deploy/AgentConfig/ClaudeStatusLine --status-line --input '{
	"cwd": "/opt/Source/AgentConfig"
	, "model": {"display_name": "Opus 5"}
	, "cost": {"total_cost_usd": 0.42, "total_lines_added": 128, "total_lines_removed": 31}
}'
```

## Structure

| File | Contents |
| --- | --- |
| `Source/Main.cpp` | `CApplication` entry point, hands the actor factory to `fg_RunApp`. |
| `Source/AgentConfig_App_ClaudeStatusLine.h/.cpp` | `CClaudeStatusLineActor`, the `CDistributedToolAppActor` for the app. |
| `Source/AgentConfig_App_ClaudeStatusLine_CommandLine.cpp` | Program description, plus the base class hook that creates the `Tools` section. |
| `Source/AgentConfig_App_ClaudeStatusLine_Tool.h/.cpp` | `CStatusLineTool`, the `CDistributedTool` that registers `--status-line`, `--refresh`, and `--install`. |
| `Source/AgentConfig_App_ClaudeStatusLine_Render.h/.cpp` | The bars and the line, from the status document and the cache. |
| `Source/AgentConfig_App_ClaudeStatusLine_Cache.h/.cpp` | `~/.claude/statusline-cache.json`, read on every render and written by the refresh. |
| `Source/AgentConfig_App_ClaudeStatusLine_Install.h/.cpp` | The status line command `--install` writes, for the shell Claude Code runs it with. |
| `Source/AgentConfig_App_ClaudeStatusLine_Refresh.h/.cpp` | The usage API and codex app-server lookups behind `--refresh`. |
| `Test/` | Tests of the render, in the `Tests` workspace. |

`CDistributedToolAppActor` finds every `CDistributedTool` implementation through the runtime type
registry, so a second tool is added by writing another `CDistributedTool` subclass with a
`DMibRuntimeClassNamed` registration next to `CStatusLineTool`.

## The rendered line

```
[context bar] <n>%  <Scoped initial>:<n>% O:<n>% [split burn bar] <when it runs out> <time to reset>  Cx:<n>% [burn bar, carrying when it runs out] <time to reset>
```

Everything the line needs comes from the status document and one cache file, so a render is a
single process: read stdin, read cache, print. Network and process spawning happen in a detached
`--refresh` child, never on the render path; see [Performance](#performance).

Both weekly limits are shown: the one the usage API scopes to a model, such as Fable, with that
model's initial, and the one shared by all models. The shared one is labelled `O`, what Opus draws
from, while the model in use has the scoped limit, and with the initial of the model in use
otherwise. They share a split burn bar: the top half of each cell is the scoped limit and the bottom
half the shared one, in steps of half a cell. The label of the limit the model in use draws from is
blue, and the times after the bar are that limit's. The percent of the limit that runs out first at
the pace so far is a brighter grey than the other's.

The burn bar compares quota spent against time elapsed in the window: half full means the quota is
on track to land exactly on 100% at the reset. When the pace runs the quota out before the window
resets, the time until that is carried inside the filled part of the bar, which grows until it holds
the text with a column to spare on either side. A bar that has saturated at 200% cannot show how far
past the pace it is, so it puts the real ratio in front of the same text. A quota that is used up
is a full red bar that says `out`, or for Codex with a credit balance, the credits left as
`<n> cred`. The time after the bar is always the time until the window resets.

Half a row cannot hold text, so the split bar puts what a bar would carry after it instead, in the
colour of that limit's pace. A cell has only two colours, so where both halves are filled in
different colours, a half filled half is shown full.

An account billed by usage, such as an enterprise seat with a spend cap, has no weekly limits. Its line shows
the share of the cap spent so far this month instead, as `$:<n>%`, with a burn bar paced over the calendar
month in UTC, which is when the cap is taken to reset: the usage API does not say.

```
[context bar] <n>%  $:<n>% [burn bar, carrying when it runs out] <time to reset>
```

The Codex section is left out when codex is not set up: no `codex` executable at `~/.local/bin` or on the path, and
no `~/.codex/sessions`. A refresh then succeeds without it.

- Context window and the all-models weekly limit: the status document on stdin.
- Per-model weekly limit: `api.anthropic.com/api/oauth/usage`, with the OAuth token from the macOS
  Keychain (or `~/.claude/.credentials.json`).
- Monthly spend cap: the `spend` object of the same usage API answer.
- Codex weekly limit: `codex app-server` -> `account/rateLimits/read`, the same live call Codex's own
  `/status` makes. Falls back to the last snapshot in the newest session rollout.

A `~` after a percent marks usage that may be out of date: Claude usage from the cache when no refresh has succeeded
for an hour, and a Codex rollout snapshot older than 12 hours. A failed Claude refresh keeps the usage it had and
records why it failed as `lastError` in the Claude entry of `~/.claude/statusline-cache.json`.

The autocompact zone at the end of the context bar is what Claude Code keeps free: 20k tokens for output, or
`CLAUDE_CODE_MAX_OUTPUT_TOKENS` when that is less, which makes its effective window, and a 13k buffer below that,
where compaction starts. `CLAUDE_AUTOCOMPACT_PCT_OVERRIDE` is the percent of the effective window at which compaction
starts instead, so on a 200k window `80` starts it at 144k. Claude Code ignores an override that would start compaction
later than the buffer does, and so does the line.

## Performance

A render costs a few milliseconds from spawn to exit, measured over 1000 renders with the cache fresh,
in Release builds:

| Host | Median | Fastest | CPU time | `/usr/bin/true` |
| --- | --- | --- | --- | --- |
| Linux, Intel Core i7-13700K | 1.9–2.0 ms | 1.8 ms | 2.2 ms | 0.14 ms |
| macOS, Apple M5 Pro | 5.9–6.0 ms | 5.5 ms | 4.2 ms | 0.57 ms |
| macOS, Apple M1 Max | 9.5–10.2 ms | 8.7 ms | 7.3 ms | 1.0 ms |

`/usr/bin/true` is the cost of starting any process on the same host. Starting a process accounts for at most 1 ms
of the gap between Linux and macOS. Most of it is the CPU time the render itself takes on macOS.

A render starts two threads besides the main thread: the one pool thread the app asks for, and a blocking actor that
loads the distributed app's config and state databases. The [repository README](../../README.md#status-line-overhead)
compares a render with bun and node.
