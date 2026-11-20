// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>

namespace NMib::NAgentConfig::NDesktopHelper
{
	// Runs in the desktop session as a daemon. Commands from a command line in any other session of the same user,
	// such as an agent connected over SSH, are forwarded to it and launch their processes here.
	struct CDesktopHelperActor : public NConcurrency::CDistributedAppActor
	{
		CDesktopHelperActor();
		~CDesktopHelperActor();

	private:
		void fp_BuildCommandLine(NConcurrency::CDistributedAppCommandLineSpecification &o_CommandLine) override;
		NConcurrency::TCFuture<void> fp_FinalizeCommandLine(NStorage::TCSharedPointer<NConcurrency::CDistributedAppCommandLineSpecification> _pCommandLine) override;

		NConcurrency::TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		NConcurrency::TCFuture<void> fp_StopApp() override;

		auto fp_RunInDesktop
			(
				NStorage::TCSharedPointer<NConcurrency::CCommandLineControl> _pCommandLine
				, NStr::CStr _Executable
				, NContainer::TCVector<NStr::CStr> _Arguments
				, NStr::CStr _WorkingDirectory
				, NContainer::TCVector<NStr::CStr> _EnvironmentNames
			)
			-> NConcurrency::TCFuture<uint32>
		;

		uint64 mp_iNextRun = 0; // Tags the audit entries of one run, since runs from several callers interleave in the log
	};
}
