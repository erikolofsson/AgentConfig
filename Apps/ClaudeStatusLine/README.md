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
| `Source/AgentConfig_App_ClaudeStatusLine_Refresh.h/.cpp` | The usage API and codex app-server lookups behind `--refresh`. |

`CDistributedToolAppActor` finds every `CDistributedTool` implementation through the runtime type
registry, so a second tool is added by writing another `CDistributedTool` subclass with a
`DMibRuntimeClassNamed` registration next to `CStatusLineTool`.

## The rendered line

```
[context bar] <n>%  <Model initial>:<n>% [burn bar, carrying when it runs out] <time to reset>  Cx:<n>% [burn bar] <time to reset>
```

Everything the line needs comes from the status document and one cache file, so a render is a
single process: read stdin, read cache, print. Network and process spawning happen in a detached
`--refresh` child, never on the render path.

The weekly limit shown is the one that applies to the model in use: the model's own scoped limit
when the usage API reports one for it, otherwise the limit shared by all models.

The burn bar compares quota spent against time elapsed in the window: half full means the quota is
on track to land exactly on 100% at the reset. When the pace runs the quota out before the window
resets, the time until that is carried inside the filled part of the bar, which grows until it holds
the text with a column to spare on either side. A bar that has saturated at 200% cannot show how far
past the pace it is, so it puts the real ratio in front of the same text. The time after the bar is
always the time until the window resets.

- Context window and the all-models weekly limit: the status document on stdin.
- Per-model weekly limit: `api.anthropic.com/api/oauth/usage`, with the OAuth token from the macOS
  Keychain (or `~/.claude/.credentials.json`).
- Codex weekly limit: `codex app-server` -> `account/rateLimits/read`, the same live call Codex's own
  `/status` makes. Falls back to the last snapshot in the newest session rollout.

`CLAUDE_AUTOCOMPACT_PCT_OVERRIDE` overrides the autocompact zone, the way it does for Claude Code.
