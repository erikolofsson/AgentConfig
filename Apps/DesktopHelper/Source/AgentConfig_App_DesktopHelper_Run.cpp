// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/Daemon/Daemon>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Time/Stopwatch>

#include "AgentConfig_App_DesktopHelper.h"

namespace NMib::NAgentConfig::NDesktopHelper
{
	using namespace NMib::NStr;
	using namespace NMib::NConcurrency;
	using namespace NMib::NProcess;

	namespace
	{
		enum class ERunState
		{
			mc_Running
			, mc_Stopping
			, mc_Done
		};

		constexpr fp64 gc_StopGracePeriod = 5.0;

		// Stops the process group, then kills what is left of the process tree once the process has stopped or ignored the stop
		// for the grace period. The stop resolves when the process exits, while descendants that ignore it can keep running
		TCFuture<void> fg_StopRun(TCActor<CProcessLaunchActor> _ProcessLaunchActor, CDistributedAppAuditor _Auditor, uint64 _iRun)
		{
			auto StopResult = co_await _ProcessLaunchActor(&CProcessLaunchActor::f_StopProcessGroup).f_Timeout(gc_StopGracePeriod, "Timed out stopping the process").f_Wrap();

			if (!StopResult)
				_Auditor.f_Warning("[Run {}] The process did not stop: {}. Killing it and its descendants"_f << _iRun << StopResult.f_GetExceptionStr());

			co_await _ProcessLaunchActor(&CProcessLaunchActor::f_TerminateProcessTree);

			co_return {};
		}
	}

	auto CDesktopHelperActor::fp_RunInDesktop(NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine, CStr _Executable, NContainer::TCVector<CStr> _Arguments, CStr _WorkingDirectory)
		-> TCFuture<uint32>
	{
		// The calling host is only known before the first suspension
		auto Auditor = f_Auditor();

		uint64 iRun = mp_iNextRun++;
		NContainer::TCVector<CStr> Command{_Executable};
		Command.f_Insert(_Arguments);
		CStr CommandLine = CProcessLaunchParams::fs_GetParamsBash(Command);

		Auditor.f_Info("[Run {}] Running in '{}': {}"_f << iRun << _WorkingDirectory << CommandLine);

#ifdef DPlatformFamily_macOS
		// On macOS the user daemon feature of the session is whether it holds the graphical login. Elsewhere it is whether user
		// daemons can be installed, which says nothing about desktop access
		if (!(NDaemon::CDaemon::fs_SessionSupportedFeatures() & NDaemon::EDaemonFeature_LocalUserDaemon))
		{
			co_return Auditor.f_Exception
				(
					"[Run {}] The desktop helper is running in a session without access to the desktop. Ask the user to stop it and start "
					"it from a terminal in their desktop session instead."_f
					<< iRun
				)
			;
		}
#endif

		CProcessLaunchActor::CLaunch Launch = CProcessLaunchParams::fs_LaunchExecutable(_Executable, _Arguments, _WorkingDirectory, nullptr);
		Launch.m_Params.m_bAllowExecutableLocate = true;
		// Cancelling stops the group, which includes the processes the executable started. Only cancelling does, as processes that
		// were started to keep running must survive the executable exiting
		Launch.m_Params.m_bCreateNewProcessGroup = true;
		// Prompts and progress output reach the caller without waiting for a newline
		Launch.m_bWholeLineOutput = false;

		TCPromiseFuturePair<void> LaunchedPromise;
		TCPromiseFuturePair<uint32> ExitedPromise;
		Launch.m_Params.m_fOnStateChange = [LaunchedPromise = fg_Move(LaunchedPromise.m_Promise), ExitedPromise = fg_Move(ExitedPromise.m_Promise)]
			(CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
			{
				switch (_State.f_GetTypeID())
				{
				case EProcessLaunchState_Launched: LaunchedPromise.f_SetResult(); break;
				case EProcessLaunchState_LaunchFailed: LaunchedPromise.f_SetException(DMibErrorInstance(_State.f_Get<EProcessLaunchState_LaunchFailed>())); break;
				case EProcessLaunchState_Exited: ExitedPromise.f_SetResult(_State.f_Get<EProcessLaunchState_Exited>()); break;
				}
			}
		;

		Launch.m_Params.m_fOnOutputBinary = [_pCommandLine](EProcessLaunchOutputType _OutputType, NContainer::CIOByteVector const &_Output)
			{
				if (_OutputType == EProcessLaunchOutputType_StdOut)
					*_pCommandLine += _Output;
				else
					*_pCommandLine %= _Output;
			}
		;

		// The errors and termination messages of the launch itself
		Launch.m_Params.m_fOnOutput = [_pCommandLine](EProcessLaunchOutputType _OutputType, CStr const &_Output)
			{
				if (!_Output.f_IsEmpty())
					*_pCommandLine %= _Output;
			}
		;

		TCActor<CProcessLaunchActor> ProcessLaunchActor(fg_Construct());
		auto DestroyLaunch = co_await fg_AsyncDestroy(ProcessLaunchActor);

		// The functor is also dropped once the run is over, which is not the caller going away. A stop that reaches the launch actor
		// before the process exists does nothing, so the run checks for one after launching
		NStorage::TCSharedPointer<ERunState> pRunState = fg_Construct(ERunState::mc_Running);

		// The command line drops the functor when its process goes away without cancelling, such as when an SSH
		// connection is lost, and the process should not outlive it
		auto CancellationSubscription = co_await _pCommandLine->f_RegisterForCancellation
			(
				g_ActorFunctor
				(
					g_ActorSubscription / [ProcessLaunchActor, Auditor, iRun, pRunState]() -> TCFuture<void>
					{
						if (*pRunState == ERunState::mc_Done)
							co_return {};

						*pRunState = ERunState::mc_Stopping;
						Auditor.f_Warning("[Run {}] The caller went away, stopping the process"_f << iRun);
						fg_StopRun(ProcessLaunchActor, Auditor, iRun).f_DiscardResult();
						co_return {};
					}
				)
				/ [ProcessLaunchActor, Auditor, iRun, pRunState]() -> TCFuture<bool>
				{
					*pRunState = ERunState::mc_Stopping;
					Auditor.f_Info("[Run {}] Cancelled by the caller"_f << iRun);
					co_await fg_StopRun(ProcessLaunchActor, Auditor, iRun);
					co_return true;
				}
			)
		;

		// Declared after the subscription so that it runs before the subscription is released
		auto SetDone = g_OnScopeExit / [pRunState]
			{
				*pRunState = ERunState::mc_Done;
			}
		;

		if (*pRunState == ERunState::mc_Stopping)
			co_return Auditor.f_Exception("[Run {}] Cancelled before the launch"_f << iRun);

		NTime::CStopwatch Stopwatch(true);

		auto LaunchSubscription = co_await ProcessLaunchActor(&CProcessLaunchActor::f_Launch, fg_Move(Launch), fg_ThisActor(this));

		auto LaunchResult = co_await fg_Move(LaunchedPromise.m_Future).f_Wrap();
		if (!LaunchResult)
			co_return Auditor.f_Exception("[Run {}] Failed to launch executable: {}"_f << iRun << LaunchResult.f_GetExceptionStr());

		if (*pRunState == ERunState::mc_Stopping)
			co_await fg_StopRun(ProcessLaunchActor, Auditor, iRun);

		auto StdInSubscription = co_await _pCommandLine->f_RegisterForStdInBinary
			(
				g_ActorFunctor / [ProcessLaunchActor, _pCommandLine, bEndOfFile = false]
				(EStdInReaderOutputType _Type, NContainer::CIOByteVector _Input, CStr _Error) mutable -> TCFuture<void>
				{
					switch (_Type)
					{
					case EStdInReaderOutputType_StdIn: co_await ProcessLaunchActor(&CProcessLaunchActor::f_SendStdInBinary, fg_Move(_Input)); break;
					case EStdInReaderOutputType_GeneralError: *_pCommandLine %= _Error; break;
					case EStdInReaderOutputType_EndOfFile:
						if (!bEndOfFile)
						{
							bEndOfFile = true;
							co_await ProcessLaunchActor(&CProcessLaunchActor::f_CloseStdIn).f_Wrap() > fg_LogError("DesktopHelper", "Failed to close stdin");
						}
						break;
					}

					co_return {};
				}
				, EStdInReaderFlag_LineInput
			)
		;

		uint32 ExitStatus = co_await fg_Move(ExitedPromise.m_Future);

		Auditor.f_Info("[Run {}] Exited with status {} after {fe1} s"_f << iRun << ExitStatus << Stopwatch.f_GetTime());

		co_return ExitStatus;
	}
}
