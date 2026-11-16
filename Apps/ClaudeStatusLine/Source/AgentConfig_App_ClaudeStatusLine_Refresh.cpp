// Copyright © Erik Olofsson

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/File/File>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Web/HttpClient>

#include "AgentConfig_App_ClaudeStatusLine_Refresh.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	using namespace NMib::NStr;
	using namespace NMib::NConcurrency;

	namespace
	{
		// A >= 7 day window from a codex rate limit snapshot, tolerating both key styles
		struct CWeeklyWindow
		{
			fp64 m_Percent = 0.0;
			int64 m_ResetsAt = 0;
			int64 m_WindowSeconds = 0;
		};

		NStorage::TCOptional<CWeeklyWindow> fg_WeeklyWindow(NEncoding::CJsonSorted const *_pRateLimits)
		{
			for (ch8 const *pKey : {"primary", "secondary"})
			{
				NEncoding::CJsonSorted const *pWindow = fg_JsonMember(_pRateLimits, pKey);

				if (!pWindow || !pWindow->f_IsObject())
					continue;

				NEncoding::CJsonSorted const *pMinutes = fg_JsonMember(pWindow, "windowDurationMins");

				if (!pMinutes)
					pMinutes = fg_JsonMember(pWindow, "window_minutes");

				fp64 Minutes = fg_JsonNumber(pMinutes);

				if (Minutes < 10080.0)
					continue;

				NEncoding::CJsonSorted const *pPercent = fg_JsonMember(pWindow, "usedPercent");

				if (!pPercent)
					pPercent = fg_JsonMember(pWindow, "used_percent");

				NEncoding::CJsonSorted const *pResetsAt = fg_JsonMember(pWindow, "resetsAt");

				if (!pResetsAt)
					pResetsAt = fg_JsonMember(pWindow, "resets_at");

				NStorage::TCOptional<int64> ResetsAt = fg_ToEpoch(pResetsAt);

				if (pPercent && ResetsAt && *ResetsAt)
					return CWeeklyWindow{.m_Percent = fg_JsonNumber(pPercent), .m_ResetsAt = *ResetsAt, .m_WindowSeconds = Minutes.f_ToInt() * 60};
			}

			return {};
		}

		// The OAuth token, from the Keychain on macOS and otherwise from the credentials file
		TCFuture<CStr> fg_ReadToken()
		{
			CStr Raw;
			CStr KeychainFailure;

#ifdef DPlatformFamily_macOS
			{
				NProcess::CProcessLaunchActor::CSimpleLaunch Launch
					(
						"/usr/bin/security"
						, {"find-generic-password", "-s", "Claude Code-credentials", "-w"}
						, NFile::CFile::fs_GetCurrentDirectory()
						, NProcess::CProcessLaunchActor::ESimpleLaunchFlag_None
					)
				;

				auto Result = co_await NProcess::CProcessLaunchActor::fs_LaunchSimple(Launch).f_Wrap();

				if (Result && Result->m_ExitCode == 0)
					Raw = Result->f_GetStdOut();
				else if (Result)
					KeychainFailure = "Keychain: security exited with {}: {}. "_f << Result->m_ExitCode << Result->f_GetStdErr().f_Trim();
				else
					KeychainFailure = "Keychain: {}. "_f << Result.f_GetExceptionStr();
			}
#endif

			if (Raw.f_IsEmpty())
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				CStr Path = fg_GetHomeDirectory() / ".claude/.credentials.json";

				auto FileResult = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [Path]
						{
							return NFile::CFile::fs_ReadStringFromFile(Path, true);
						}
					)
					.f_Wrap()
				;

				if (!FileResult)
					co_return DMibErrorInstance(fg_Format("No OAuth token. {}Credentials file: {}", KeychainFailure, FileResult.f_GetExceptionStr()));

				Raw = *FileResult;
			}

			NEncoding::CJsonSorted Json;
			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Could not parse the credentials");

				Json = NEncoding::CJsonSorted::fs_FromString(Raw, "credentials");
			}

			NEncoding::CJsonSorted const *pToken = fg_JsonMember(fg_JsonMember(&Json, "claudeAiOauth"), "accessToken");

			if (!pToken || !pToken->f_IsString() || pToken->f_String().f_IsEmpty())
				co_return DMibErrorInstance("The credentials carry no claudeAiOauth.accessToken");

			co_return pToken->f_String();
		}

		// Live read over the app-server's JSON-RPC stdio protocol
		TCFuture<CCodexCache> fg_CodexFromAppServer()
		{
			CStr Executable;
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				Executable = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [Installed = fg_GetHomeDirectory() / ".local/bin/codex"]
						{
							return NFile::CFile::fs_FileExists(Installed, NFile::EFileAttrib_File) ? Installed : CStr{};
						}
					)
				;
			}

			// Without the installed binary, codex is looked up on the path
			bool bLocate = Executable.f_IsEmpty();

			if (bLocate)
				Executable = "codex";

			// Started resolves when the process is running, Answer when the rate limits are in.
			// Both carry the failure when the launch fails or the server exits early
			TCPromise<void> Started;
			TCPromise<CCodexCache> Answer;
			auto fFail = [Started, Answer](NException::CExceptionPointer const &_pException)
				{
					if (!Started.f_IsSet())
						Started.f_SetException(_pException);

					if (!Answer.f_IsSet())
						Answer.f_SetException(_pException);
				}
			;

			NProcess::CProcessLaunchActor::CLaunch Launch
				{
					NProcess::CProcessLaunchParams::fs_LaunchExecutable(Executable, {"app-server"}, NFile::CFile::fs_GetCurrentDirectory(), nullptr)
				}
			;

			Launch.m_Params.m_bAllowExecutableLocate = bLocate;
			// A stop is enough, the server exits on it; nothing waits for that exit
			Launch.m_DestructFlags = NProcess::EProcessLaunchCloseFlag_StopProcess;
			Launch.m_Params.m_fOnStateChange = [Started, fFail](NProcess::CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						if (!Started.f_IsSet())
							Started.f_SetResult();
						break;
					case NProcess::EProcessLaunchState_LaunchFailed:
						fFail(DMibErrorInstance(fg_Format("Launching codex app-server failed: {}", _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>())));
						break;
					case NProcess::EProcessLaunchState_Exited:
						fFail(DMibErrorInstance(fg_Format("codex app-server exited with {} before answering", _State.f_Get<NProcess::EProcessLaunchState_Exited>())));
						break;
					}
				}
			;
			Launch.m_Params.m_fOnOutput = [Answer](NProcess::EProcessLaunchOutputType _OutputType, CStr const &_Output)
				{
					if (_OutputType != NProcess::EProcessLaunchOutputType_StdOut || Answer.f_IsSet())
						return;

					NEncoding::CJsonSorted Message;

					try
					{
						Message = NEncoding::CJsonSorted::fs_FromString(_Output);
					}
					catch (NException::CException const &)
					{
						// Not one of the protocol's JSON lines
						return;
					}

					NEncoding::CJsonSorted const *pID = fg_JsonMember(&Message, "id");

					if (!pID || !pID->f_IsInteger() || pID->f_Integer() != 1)
						return;

					NStorage::TCOptional<CWeeklyWindow> Window = fg_WeeklyWindow(fg_JsonMember(fg_JsonMember(&Message, "result"), "rateLimits"));

					if (!Window)
					{
						Answer.f_SetException(DMibErrorInstance(fg_Format("codex app-server answered without a weekly window: {}", _Output)));
						return;
					}

					int64 Now = fg_UnixNow();
					TCAsyncResult<CCodexCache> Result;

					Result.f_SetResult
						(
							CCodexCache
							{
								.m_Percent = Window->m_Percent
								, .m_ResetsAt = Window->m_ResetsAt
								, .m_WindowSeconds = Window->m_WindowSeconds
								, .m_FetchedAt = Now
								, .m_SnapshotAt = Now
							}
						)
					;
					Answer.f_SetResult(fg_Move(Result));
				}
			;

			TCActor<NProcess::CProcessLaunchActor> LaunchActor = fg_ConstructActor<NProcess::CProcessLaunchActor>();

			auto AsyncDestroy = co_await fg_AsyncDestroy(LaunchActor);

			auto Subscription = co_await (LaunchActor(&NProcess::CProcessLaunchActor::f_Launch, Launch, fg_CurrentActor()) % "Launching codex app-server");

			// The requests go through the launch's stdin pipe, which exists once the launch has
			// reported the process as running; written earlier they are dropped
			co_await (Started.f_Future().f_Timeout(10.0, "Not running after 10 seconds") % "Starting codex app-server");

			CStr Request =
				"{\"id\":0,\"method\":\"initialize\",\"params\":{\"clientInfo\":"
				"{\"name\":\"statusline\",\"title\":\"statusline\",\"version\":\"0.1.0\"}}}\n"
				"{\"method\":\"initialized\",\"params\":null}\n"
				"{\"id\":1,\"method\":\"account/rateLimits/read\",\"params\":null}\n"
			;

			LaunchActor(&NProcess::CProcessLaunchActor::f_SendStdIn, CStrIO(Request)).f_DiscardResult();

			co_return co_await (Answer.f_Future().f_Timeout(10.0, "No answer within 10 seconds") % "Reading account/rateLimits/read from codex app-server");
		}

		// Fallback: last rate_limits snapshot in the newest session rollout. Blocking file work
		// that never suspends; the wrapped result carries a failure, its own or one that escaped
		TCWrapped<CCodexCache> fg_CodexFromRolloutBlocking()
		{
			CStr Base = fg_GetHomeDirectory() / ".codex/sessions";
			NContainer::TCVector<CStr> Directories;
			auto fSortedDirectories = [](CStr const &_Path)
				{
					NContainer::TCVector<CStr> Found = NFile::CFile::fs_FindFiles(_Path / "*", NFile::EFileAttrib_Directory);

					Found.f_Sort();

					return Found;
				}
			;

			for (CStr const &Year : fSortedDirectories(Base))
			{
				for (CStr const &Month : fSortedDirectories(Year))
				{
					for (CStr const &Day : fSortedDirectories(Month))
						Directories.f_Insert(Day);
				}
			}

			struct CRollout
			{
				CStr m_Path;
				NTime::CTime m_Modified;
			};

			NContainer::TCVector<CRollout> Files;

			for (umint iDirectory = Directories.f_GetLen() > 2 ? Directories.f_GetLen() - 2 : 0; iDirectory < Directories.f_GetLen(); ++iDirectory)
			{
				try
				{
					for (CStr const &Path : NFile::CFile::fs_FindFiles(Directories[iDirectory] / "rollout-*.jsonl", NFile::EFileAttrib_File))
						Files.f_Insert(CRollout{.m_Path = Path, .m_Modified = NFile::CFile::fs_GetWriteTime(Path)});
				}
				catch (NException::CException const &)
				{
					// Directory vanished mid-scan
				}
			}

			if (Files.f_IsEmpty())
				co_return DMibErrorInstance(fg_Format("No rollout files in the newest session days under {}", Base));

			CRollout const *pNewest = &Files[0];

			for (CRollout const &File : Files)
			{
				if (File.m_Modified > pNewest->m_Modified)
					pNewest = &File;
			}

			CStr Text;
			{
				NFile::CFile File(pNewest->m_Path, NFile::EFileOpen_Read | NFile::EFileOpen_ShareAll);
				int64 Length = File.f_GetLength();
				int64 Start = fg_Max(int64(0), Length - 400000);
				NContainer::CByteVector Buffer;

				Buffer.f_SetLen(umint(Length - Start));
				File.f_SetPosition(Start);
				File.f_Read(Buffer.f_GetArray(), Buffer.f_GetLen());

				Text = CStr((ch8 const *)Buffer.f_GetArray(), Buffer.f_GetLen());
			}

			NStorage::TCOptional<CWeeklyWindow> Best;
			CStr BestTimestamp;

			for (CStr const &Line : Text.f_Split("\n"))
			{
				if (Line.f_Find("\"rate_limits\"") < 0)
					continue;

				NEncoding::CJsonSorted Entry;

				try
				{
					Entry = NEncoding::CJsonSorted::fs_FromString(Line);
				}
				catch (NException::CException const &)
				{
					// A line cut by the 400 KB window, or not JSON
					continue;
				}

				NStorage::TCOptional<CWeeklyWindow> Window = fg_WeeklyWindow(fg_JsonMember(fg_JsonMember(&Entry, "payload"), "rate_limits"));

				if (!Window)
					continue;

				Best = Window;
				BestTimestamp.f_Clear();

				NEncoding::CJsonSorted const *pTimestamp = fg_JsonMember(&Entry, "timestamp");

				if (pTimestamp && pTimestamp->f_IsString())
					BestTimestamp = pTimestamp->f_String();
			}

			if (!Best)
				co_return DMibErrorInstance(fg_Format("No rate_limits snapshot with a weekly window in {}", pNewest->m_Path));

			NStorage::TCOptional<int64> SnapshotAt;

			if (!BestTimestamp.f_IsEmpty())
			{
				NEncoding::CJsonSorted Timestamp;

				Timestamp = BestTimestamp;
				SnapshotAt = fg_ToEpoch(&Timestamp);
			}

			co_return CCodexCache
				{
					.m_Percent = Best->m_Percent
					, .m_ResetsAt = Best->m_ResetsAt
					, .m_WindowSeconds = Best->m_WindowSeconds
					, .m_FetchedAt = fg_UnixNow()
					, .m_SnapshotAt = SnapshotAt ? *SnapshotAt : 0
				}
			;
		}

		TCFuture<CCodexCache> fg_CodexFromRollout()
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			// The dispatched coroutine unwraps the result, so the future carries the value or the failure
			co_return co_await
				(
					g_Dispatch(BlockingActorCheckout) / []() -> TCFuture<CCodexCache>
					{
						co_return co_await fg_CodexFromRolloutBlocking();
					}
					% "Reading the codex session rollouts"
				)
			;
		}
	}

	TCFuture<CClaudeCache> fg_FetchClaudeUsage(CStr _SeenWeekly)
	{
		CStr Token = co_await (fg_ReadToken() % "Reading the OAuth token");

		TCActor<NWeb::CHttpClientActor> HttpClient = fg_Construct();
		NContainer::TCMap<CStr, CStr> Headers;

		Headers["Authorization"] = "Bearer {}"_f << Token;
		Headers["anthropic-beta"] = "oauth-2025-04-20";
		Headers["Content-Type"] = "application/json";

		NWeb::CHttpClientActor::CResult Result = co_await
			(
				HttpClient(&NWeb::CHttpClientActor::f_Get, CStr("https://api.anthropic.com/api/oauth/usage"), Headers).f_Timeout(5.0, "No answer within 5 seconds")
				% "Requesting api.anthropic.com/api/oauth/usage"
			)
		;

		if (Result.m_StatusCode < 200 || Result.m_StatusCode >= 300)
			co_return DMibErrorInstance(fg_Format("The usage API answered {} {}: {}", Result.m_StatusCode, Result.m_StatusMessage, Result.m_Body.f_Trim()));

		NEncoding::CJsonSorted Data;
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Could not parse the usage API answer");

			Data = NEncoding::CJsonSorted::fs_FromString(Result.m_Body, "usage");
		}

		CClaudeCache Claude;

		Claude.m_FetchedAt = fg_UnixNow();
		Claude.m_SeenWeekly = _SeenWeekly;

		NEncoding::CJsonSorted const *pLimits = fg_JsonMember(&Data, "limits");

		// The weekly limits the answer carries: one scoped to a model, and the one covering them all
		if (pLimits && pLimits->f_IsArray())
		{
			for (NEncoding::CJsonSorted const &Entry : pLimits->f_Array())
			{
				NEncoding::CJsonSorted const *pKind = fg_JsonMember(&Entry, "kind");

				if (!pKind || !pKind->f_IsString())
					continue;

				bool bScoped = pKind->f_String() == "weekly_scoped";

				if (!bScoped && pKind->f_String() != "weekly_all")
					continue;

				CUsageLimit Limit;
				NEncoding::CJsonSorted const *pName = fg_JsonMember(fg_JsonMember(fg_JsonMember(&Entry, "scope"), "model"), "display_name");

				if (pName && pName->f_IsString())
					Limit.m_Name = pName->f_String();

				Limit.m_Percent = fg_JsonNumber(fg_JsonMember(&Entry, "percent"));

				NStorage::TCOptional<int64> ResetsAt = fg_ToEpoch(fg_JsonMember(&Entry, "resets_at"));

				Limit.m_ResetsAt = ResetsAt ? *ResetsAt : 0;

				if (bScoped)
					Claude.m_Scoped = Limit;
				else
					Claude.m_AllModels = Limit;
			}
		}

		co_return Claude;
	}

	TCFuture<CCodexCache> fg_FetchCodexUsage()
	{
		auto FromAppServer = co_await fg_CodexFromAppServer().f_Wrap();

		if (FromAppServer)
			co_return *FromAppServer;

		auto FromRollout = co_await fg_CodexFromRollout().f_Wrap();

		if (FromRollout)
			co_return *FromRollout;

		// Both failed: reported together
		NContainer::TCVector<TCAsyncResult<void>> Failures;

		Failures.f_Insert(fg_Outcome(FromAppServer));
		Failures.f_Insert(fg_Outcome(FromRollout));

		co_await (fg_Move(Failures) | (g_Unwrap % "Codex usage unavailable"));

		co_return DMibErrorInstance("Codex usage unavailable");
	}

	TCAsyncResult<void> fg_Outcome(CAsyncResult const &_Result)
	{
		TCAsyncResult<void> Outcome;

		if (_Result)
			Outcome.f_SetResult();
		else
			Outcome.f_SetException(_Result);

		return Outcome;
	}
}
