// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/RuntimeType>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/File/File>
#include <Mib/Process/ProcessLaunch>

#include "AgentConfig_App_ClaudeStatusLine_Tool.h"
#include "AgentConfig_App_ClaudeStatusLine_Install.h"
#include "AgentConfig_App_ClaudeStatusLine_Refresh.h"
#include "AgentConfig_App_ClaudeStatusLine_Render.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	using namespace NMib::NStr;
	using namespace NMib::NConcurrency;

	namespace
	{
		constexpr umint gc_MaxStoredErrorLength = 500;

		// Starts `--refresh` for the stale targets as a child that outlives this process. Its stdio
		// is pipes this process closes right away, so it never holds the stdout Claude Code reads
		void fg_SpawnRefresh(NContainer::TCVector<CStr> const &_Targets, CStr const &_SeenWeekly, int64 _Observed)
		{
			CStr Executable = NFile::CFile::fs_GetProgramPath();
			CStr Targets;

			for (CStr const &Target : _Targets)
			{
				if (!Targets.f_IsEmpty())
					Targets += ",";

				Targets += Target;
			}

			NProcess::CProcessLaunchParams Params = NProcess::CProcessLaunchParams::fs_LaunchExecutable
				(
					Executable
					, {"--refresh", "--targets", Targets, "--unchanged-since", "{}"_f << _Observed}
					, NFile::CFile::fs_GetCurrentDirectory()
					, nullptr
				)
			;

			Params.m_Environment["STATUSLINE_SEEN_WEEKLY"] = _SeenWeekly;
			Params.m_bCreateNewProcessGroup = true;
			// Spawned on this thread rather than a launch thread; with no close flags the launch
			// returns as soon as the child is running and leaves it be
			Params.m_bThreaded = false;

			NProcess::CProcessLaunch Launch(Params, NProcess::EProcessLaunchCloseFlag_None);

			Launch.f_Start();
		}
	}

	DMibRuntimeClassNamed(NMib::NConcurrency::CDistributedTool, CStatusLineTool, NMib::NAgentConfig::NClaudeStatusLine::CStatusLineTool);

	CStatusLineTool::CStatusLineTool()
	{
	}

	CStatusLineTool::~CStatusLineTool()
	{
	}

	void CStatusLineTool::f_Register
		(
			TCActor<CDistributedToolAppActor> const &_ToolActor
			, CDistributedAppCommandLineSpecification::CSection &o_ToolsSection
			, CDistributedAppCommandLineSpecification &o_CommandLine
			, CStr const &_ClassName
		)
	{
		o_ToolsSection.f_RegisterCommand
			(
				{
					"Names"_o= _o["--status-line"]
					, "Description"_o= "Render the Claude Code status line.\n"
					"Reads the status JSON document from stdin, as Claude Code supplies it to a statusLine command.\n"
					"The per-model and Codex weekly limits come from ~/.claude/statusline-cache.json, which a detached "
					"--refresh child keeps fresh.\n"
					, "Output"_o= "A single line of text with ansi escape codes."
					, "Options"_o=
					{
						"Input?"_o=
						{
							"Names"_o= _o["--input"]
							, "Default"_o= ""
							, "Description"_o= "Use this JSON document instead of reading one from stdin. Useful while iterating on the layout.\n"
						}
					}
				}
				, [this](NEncoding::CEJsonSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_RunStatusLine(fg_Move(_pCommandLine), _Params["Input"].f_String());
				}
			)
		;

		o_ToolsSection.f_RegisterCommand
			(
				{
					"Names"_o= _o["--refresh"]
					, "Description"_o= "Refresh the usage cache the status line renders from.\n"
					"The status line starts this itself as a detached child whenever an entry is stale.\n"
					, "Options"_o=
					{
						"Targets?"_o=
						{
							"Names"_o= _o["--targets"]
							, "Default"_o= "claude,codex"
							, "Description"_o= "Comma separated entries to refresh: claude (the per-model weekly limit from the usage API) "
							"and codex (the Codex weekly limit from codex app-server, left out when codex is not set up).\n"
						}
						, "UnchangedSince?"_o=
						{
							"Names"_o= _o["--unchanged-since"]
							, "Default"_o= "0"
							, "Description"_o= "Refresh an entry only while nothing has touched it since this Unix time. The status line "
							"passes the time it read the cache, so sessions sharing the cache refresh an entry once between them "
							"rather than once each. Zero, the default, refreshes whatever is asked for.\n"
						}
					}
				}
				, [this](NEncoding::CEJsonSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_RunRefresh(_Params["Targets"].f_String(), _Params["UnchangedSince"].f_String().f_ToIntExact(int64(0)));
				}
			)
		;

		o_ToolsSection.f_RegisterCommand
			(
				{
					"Names"_o= _o["--install"]
					, "Description"_o= "Install this executable as the Claude Code status line.\n"
					"Sets statusLine in the Claude Code settings to run `<this executable> --status-line`, keeping "
					"every other setting as it is.\n"
					, "Options"_o=
					{
						"Settings?"_o=
						{
							"Names"_o= _o["--settings"]
							, "Default"_o= ""
							, "Description"_o= "The settings file to change. Defaults to ~/.claude/settings.json.\n"
						}
						, "RefreshInterval?"_o=
						{
							"Names"_o= _o["--refresh-interval"]
							, "Default"_o= "10"
							, "Description"_o= "Seconds between the timed re-runs Claude Code makes on top of its own triggers, which keeps "
							"the burn bars and the times to reset moving while a session sits idle. Zero leaves the setting out.\n"
						}
						, "Shell?"_o=
						{
							"Names"_o= _o["--shell"]
							, "Default"_o= "auto"
							, "Type"_o= NEncoding::fg_UserTypeOrdered("$OneOf", _jo["auto", "bash", "powershell"])
							, "Description"_o= "The shell the command is written for. Auto is the one Claude Code would use here: Bash, or on "
							"Windows Git Bash when it is found the way Claude Code finds it, and PowerShell otherwise. Name it when Claude Code "
							"starts with another PATH or CLAUDE_CODE_GIT_BASH_PATH than this command line has.\n"
						}
					}
				}
				, [this](NEncoding::CEJsonSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_RunInstall(fg_Move(_pCommandLine), _Params["Settings"].f_String(), _Params["RefreshInterval"].f_String().f_ToIntExact(int64(10)), _Params["Shell"].f_String());
				}
			)
		;
	}

	TCFuture<uint32> CStatusLineTool::fp_RunStatusLine(NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine, CStr _OverrideInput)
	{
		// The cache read goes to the blocking actor first thing, so it overlaps reading stdin
		TCFuture<CStatusCache> CacheRead;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			CacheRead = g_Dispatch(BlockingActorCheckout) / []
				{
					return fg_ReadCache();
				}
			;
		}

		CStr Document = _OverrideInput;

		if (Document.f_IsEmpty())
		{
			NContainer::CIOByteVector StdIn = co_await _pCommandLine->f_ReadBinary();

			Document = CStr(StdIn.f_GetArray(), StdIn.f_GetLen());
		}

		NEncoding::CJsonSorted Json;

		try
		{
			Json = NEncoding::CJsonSorted::fs_FromString(Document.f_IsEmpty() ? CStr("{}") : Document);
		}
		catch (NException::CException const &)
		{
			// Degrade to an empty document rather than fail the line
			Json = NEncoding::EJsonType_Object;
		}

		CStatusLineInput Input = CStatusLineInput::fs_FromJson(Json);
		CStatusCache Cache = co_await fg_Move(CacheRead);
		CAutocompactSettings Autocompact
			{
				.m_PctOverride = fg_GetSys()->f_GetEnvironmentVariable("CLAUDE_AUTOCOMPACT_PCT_OVERRIDE").f_Trim().f_ToFloatExact(fp64(0.0))
				, .m_MaxOutputTokens = fg_ParseClaudeCodeInteger(fg_GetSys()->f_GetEnvironmentVariable("CLAUDE_CODE_MAX_OUTPUT_TOKENS")).f_Get(int64(0))
			}
		;
		int64 Now = fg_UnixNow();
		CRenderResult Render = fg_RenderStatusLine(Input, Cache, Now, Autocompact);

		// The spawn is the slow part of a render that finds something stale, so it goes to the
		// blocking actor the app already started, as soon as the targets are known, and overlaps
		// the output. Nothing waits for it here: the blocking actor is joined at shutdown
		if (!Render.m_StaleTargets.f_IsEmpty())
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			(
				g_Dispatch(BlockingActorCheckout) / [Targets = Render.m_StaleTargets, SeenWeekly = Input.m_bWeeklyPresent ? Input.m_WeeklyText : CStr{}, Now]
				{
					// The child is handed the time this render read the cache, and claims only what
					// nothing has touched since, so several sessions finding the same entry stale
					// still refresh it once
					fg_SpawnRefresh(Targets, SeenWeekly, Now);
				}
			)
			.f_DiscardResult();
		}

		co_await _pCommandLine->f_StdOut("{}\n"_f << Render.m_Line);

		co_return 0;
	}

	TCFuture<uint32> CStatusLineTool::fp_RunInstall(NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine, CStr _SettingsPath, int64 _RefreshInterval, CStr _Shell)
	{
		if (_SettingsPath.f_IsEmpty())
			_SettingsPath = fg_GetHomeDirectory() / ".claude/settings.json";

		auto BlockingActorCheckout = fg_BlockingActor();

		CStr Command = co_await
			(
				g_Dispatch(BlockingActorCheckout)
				/ [SettingsPath = _SettingsPath, _RefreshInterval, _Shell]
				{
					// Finding the shell looks for files on the PATH, which a network directory can hold up
					EStatusLineShell Shell = _Shell == "bash" ? EStatusLineShell::mc_Bash : _Shell == "powershell" ? EStatusLineShell::mc_PowerShell : fg_StatusLineShell();
					CStr Command = fg_StatusLineCommand(NFile::CFile::fs_GetProgramPath(), Shell);

					// The settings keep their key order; only statusLine is replaced
					NEncoding::CJsonOrdered Settings(NEncoding::EJsonType_Object);

					if (NFile::CFile::fs_FileExists(SettingsPath, NFile::EFileAttrib_File))
						Settings = NEncoding::CJsonOrdered::fs_FromString(NFile::CFile::fs_ReadStringFromFile(SettingsPath, true), SettingsPath);

					if (!Settings.f_IsObject())
						DMibError(fg_Format("{} does not hold a JSON object", SettingsPath));

					// Built whole and put in place, so nothing an earlier install left behind survives
					NEncoding::CJsonOrdered StatusLine(NEncoding::EJsonType_Object);

					StatusLine["type"] = "command";
					StatusLine["command"] = Command;

					// Claude Code's own triggers are all conversation events, so without this the
					// line freezes between them and the times to reset stop counting down
					if (_RefreshInterval > 0)
						StatusLine["refreshInterval"] = _RefreshInterval;

					Settings["statusLine"] = fg_Move(StatusLine);

					NFile::CFile::fs_CreateDirectory(NFile::CFile::fs_GetPath(SettingsPath));

					CStr TemporaryPath = "{}.tmp.{}"_f << SettingsPath << NCryptography::fg_FastRandomID();

					NFile::CFile::fs_WriteStringToFile(TemporaryPath, Settings.f_ToString("  "), false);
					NFile::CFile::fs_AtomicReplaceFile(TemporaryPath, SettingsPath);

					return Command;
				}
				% "Installing the status line"
			)
		;

		if (_RefreshInterval > 0)
			co_await _pCommandLine->f_StdOut("Installed `{}` as the Claude Code status line in {}, re-run every {} seconds\n"_f << Command << _SettingsPath << _RefreshInterval);
		else
			co_await _pCommandLine->f_StdOut("Installed `{}` as the Claude Code status line in {}\n"_f << Command << _SettingsPath);

		co_return 0;
	}

	TCFuture<uint32> CStatusLineTool::fp_RunRefresh(CStr _Targets, int64 _Observed)
	{
		NContainer::TCVector<CStr> Requested;

		for (CStr const &Target : _Targets.f_Split(","))
		{
			CStr Trimmed = Target.f_Trim();

			if (Trimmed == "claude" || Trimmed == "codex")
				Requested.f_Insert(Trimmed);
		}

		// The entries are claimed under the cache's lock before any work, so a second session that
		// found the same entry stale finds it taken and leaves it alone
		CRefreshClaim Claim;
		int64 ClaimedAt = fg_UnixNow();
		CStr ClaimID = NCryptography::fg_FastRandomID();
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			Claim = co_await
				(
					g_Dispatch(BlockingActorCheckout)
					/ [Requested, _Observed, Now = ClaimedAt, ClaimID]
					{
						return fg_ClaimRefresh(Requested, _Observed, Now, ClaimID);
					}
					% "Claiming the usage entries to refresh"
				)
			;
		}

		bool bClaude = false;
		bool bCodex = false;

		for (CStr const &Target : Claim.m_Targets)
		{
			if (Target == "claude")
				bClaude = true;
			else if (Target == "codex")
				bCodex = true;
		}

		if (!bClaude && !bCodex)
			co_return 0;

		TCFuture<CClaudeCache> ClaudeFuture;
		TCFuture<NStorage::TCOptional<CCodexCache>> CodexFuture;

		if (bClaude)
			ClaudeFuture = fg_FetchClaudeUsage(fg_GetSys()->f_GetEnvironmentVariable("STATUSLINE_SEEN_WEEKLY"));

		if (bCodex)
			CodexFuture = fg_FetchCodexUsage();

		// Whatever succeeded is saved before the failures are reported, so one source being down
		// does not hold the other's data back
		NContainer::TCVector<TCAsyncResult<void>> Outcomes;
		NStorage::TCOptional<CClaudeCache> ClaudeRefreshed;
		NStorage::TCOptional<CCodexCache> CodexRefreshed;
		CStr ClaudeError;

		if (bClaude)
		{
			TCAsyncResult<CClaudeCache> Claude = co_await (fg_Move(ClaudeFuture) % "Claude usage").f_Wrap();

			if (Claude)
				ClaudeRefreshed = *Claude;
			else
			{
				// Every render reads the cache, and the error can carry a whole error page from the usage API. The
				// refresh's own output keeps all of it
				ClaudeError = Claude.f_GetExceptionStr();

				ClaudeError = fg_CacheErrorText(ClaudeError, gc_MaxStoredErrorLength);
			}

			Outcomes.f_Insert(fg_Outcome(Claude));
		}

		if (bCodex)
		{
			TCAsyncResult<NStorage::TCOptional<CCodexCache>> Codex = co_await (fg_Move(CodexFuture) % "Codex usage").f_Wrap();

			// Without codex set up the entry is one that has never been fetched, which the line leaves out, and
			// the attempt keeps the check to once an interval
			if (Codex && *Codex)
				CodexRefreshed = **Codex;
			else if (Codex)
				CodexRefreshed = CCodexCache{.m_AttemptedAt = ClaimedAt};

			Outcomes.f_Insert(fg_Outcome(Codex));
		}

		if (ClaudeRefreshed || CodexRefreshed || !ClaudeError.f_IsEmpty())
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					g_Dispatch(BlockingActorCheckout)
					/ [ClaudeRefreshed, ClaudeError, CodexRefreshed, ClaimID, ClaudeFetchOrder = Claim.m_ClaudeFetchOrder]
					{
						fg_SaveRefreshed(ClaudeRefreshed, ClaudeError, CodexRefreshed, ClaimID, ClaudeFetchOrder);
					}
					% "Writing the usage cache"
				)
			;
		}

		co_await (fg_Move(Outcomes) | (g_Unwrap % "Refreshing the usage cache failed"));

		co_return 0;
	}
}
