# Desktop Helper

A Malterlib distributed daemon that runs commands in the desktop session for agents that are connected
some other way, such as over SSH.

A process started from an SSH session cannot reliably do what needs the desktop: on macOS it is outside the audit
session that holds the graphical login, so it cannot open apps, show windows or reach the window server, and
on Linux it has no display or session bus. The helper runs inside the desktop session, and `--run` forwards an
executable to it from any session of the same user.

## Building

```bash
MalterlibBuildShowProgress=false ./mib build AgentConfig_DesktopHelper
```

The helper has a workspace of its own because the `AgentConfig` workspace compiles the log system out for the
status line, and the helper logs what it runs.

The executable is deployed to `/opt/Deploy/AgentConfig/DesktopHelper` on macOS (`/Deploy/...` on Linux).

## Starting the helper

From a terminal in the desktop session, run it without a command. That runs `--daemon-run-debug`, the
daemon in the foreground, until it is stopped with Ctrl-C:

```bash
/opt/Deploy/AgentConfig/DesktopHelper
```

To have it start whenever the user logs in, install it as a user daemon. User mode is the default here, so
no `--mode` is needed. Install from the desktop session too: on macOS launchd only loads a user agent from the
session that holds the graphical login.

```bash
/opt/Deploy/AgentConfig/DesktopHelper --daemon-add
/opt/Deploy/AgentConfig/DesktopHelper --daemon-remove
```

On macOS the launch agent is installed with `ProcessType` `Interactive`, so that it and the commands it runs get the
same scheduling priority as a shell in the desktop session. Without it launchd runs it as a standard daemon, at the
utility priority of 20 with the CPU frequency held down. `--daemon-add` doesn't rewrite an existing plist, so after
updating from a version without it, run `--daemon-remove` and then `--daemon-add`.

## Running commands

```bash
DesktopHelper --run -- open -a Safari
DesktopHelper --run -- screencapture -x /tmp/Screen.png
DesktopHelper --run --working-directory /tmp -- sh -c 'osascript -e "display notification \"Done\""'
```

Put `--` before the executable so that its arguments are not parsed as options of `DesktopHelper`.

- Stdin, stdout and stderr are forwarded byte for byte, and the exit status is the exit status of the executable.
  A terminal keeps its line editing and echo, so typed input is sent a line at a time and Ctrl+D ends it.
- The working directory defaults to the current directory of the caller, and a relative one is relative to it. An
  executable with a directory is relative to the working directory.
- The process gets the environment of the helper, that is of the desktop session, not that of the caller.
  An executable without a path is looked up in the PATH of the helper, which for a launchd user agent is only
  the system directories. Use an absolute path, or `/usr/bin/env NAME=VALUE ...` to set variables.
- The process runs in a process group of its own. Ctrl-C, SIGHUP or SIGTERM to the caller stops that group,
  which includes the processes the executable started. When the caller disappears without a signal, such as
  after `kill -9`, the group is stopped once the helper gives up on the caller's connection, after ten minutes.
  What is left of the process tree is then killed, once the process has exited or five seconds after the stop.
- `--run` waits for the process to exit and for its output to close. Processes the executable started are left
  running when it exits by itself, so something that keeps running can be started in the background, with its
  output redirected, such as `sh -c 'nohup server >/dev/null 2>&1 &'`, or with `open` on macOS.

When the helper is not running, `--run` fails right away and prints how to start it, so an agent can ask the user
to do that. The last line of the message says why the connection failed, such as a missing socket, or that the helper
has never run.

## Who can run commands

Any process of the same user that can connect to the socket in `~/.AgentConfig/DesktopHelper` and read the
connection ticket there can run commands in the desktop session, with the helper's environment and the macOS privacy
permissions granted to it, such as Screen Recording and Accessibility. That is what the helper is for, and it has
consequences:

- A command run through the helper is outside any sandbox of its caller. An agent sandbox, such as Claude Code's or
  Codex's, keeps a sandboxed command from using the helper only if it denies that command `~/.AgentConfig/DesktopHelper`.
  Deny the directory in the sandbox settings of agents that should not reach the desktop.
- Every process of the user, also one started over SSH, can use the permissions granted to the helper. Grant them
  only on machines where that is acceptable.

## Audit log

The helper logs every run to `~/.AgentConfig/DesktopHelper/Log/DesktopHelper.log`: the command line, the working
directory, the calling host, and then the exit status and duration, or why the run failed, was cancelled or lost
its caller. The entries of one run share a `[Run <n>]` tag, numbered from 0 each time the helper starts.

```
<5JtmDkjNATBMeH6bX [user@host/DesktopHelper_CommandLine]> [Run 1] Running in '/opt/Source/AgentConfig': sh "-c" "echo hi; exit 3"
<5JtmDkjNATBMeH6bX [user@host/DesktopHelper_CommandLine]> [Run 1] Exited with status 3 after 0.0 s
```

When logs are rotated, which is checked at most once a day, logs older than a week move to `Log/Older`. Nothing is
deleted. Each
`--run` also writes a short log of its own connection to `Log/CommandLine`.

## Structure

| File | Contents |
| --- | --- |
| `Source/Main.cpp` | `CApplication` entry point, runs `CDistributedDaemon` with user mode as the default for `--daemon-add`. |
| `Source/AgentConfig_App_DesktopHelper.h/.cpp` | `CDesktopHelperActor`, the `CDistributedAppActor`, and the settings it shares with the `--run` check. |
| `Source/AgentConfig_App_DesktopHelper_CommandLine.cpp` | `--run`, which runs in the helper and fails at once when the helper is not listening, and `--daemon-run-debug` as the default command. |
| `Source/AgentConfig_App_DesktopHelper_Run.cpp` | The process launch in the helper, with stdin, stdout, stderr and cancellation forwarded, and the audit log entries. |

The helper keeps its socket and trust databases in `~/.AgentConfig/DesktopHelper`. Commands come in over the
authenticated unix socket there, with the command line connection ticket the helper writes for its own user.
