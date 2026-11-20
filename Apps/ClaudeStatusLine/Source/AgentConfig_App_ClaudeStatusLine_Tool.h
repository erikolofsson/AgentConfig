// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/DistributedTool>

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	// A tool implementation is discovered through the runtime type registry by CDistributedToolAppActor,
	// so adding another tool to this application only means adding another CDistributedTool subclass.
	// The tool instance is owned by the application actor for the whole lifetime of the actor, which is why
	// member coroutines are allowed to keep their this pointer across suspension.
	struct CStatusLineTool : public NConcurrency::CDistributedTool, public NConcurrency::CAllowUnsafeThis
	{
		CStatusLineTool();
		~CStatusLineTool();

		void f_Register
			(
				NConcurrency::TCActor<NConcurrency::CDistributedToolAppActor> const &_ToolActor
				, NConcurrency::CDistributedAppCommandLineSpecification::CSection &o_ToolsSection
				, NConcurrency::CDistributedAppCommandLineSpecification &o_CommandLine
				, NStr::CStr const &_ClassName
			)
			override
		;

	private:
		// The render: read stdin, read the cache, print. Anything stale is handed to a detached
		// `--refresh` child, so the render never waits on the Keychain, the network, or codex
		NConcurrency::TCFuture<uint32> fp_RunStatusLine(NStorage::TCSharedPointer<NConcurrency::CCommandLineControl> _pCommandLine, NStr::CStr _OverrideInput);

		NConcurrency::TCFuture<uint32> fp_RunRefresh(NStr::CStr _Targets, int64 _Observed);

		// Points the statusLine command in the Claude Code settings at this executable
		auto fp_RunInstall(NStorage::TCSharedPointer<NConcurrency::CCommandLineControl> _pCommandLine, NStr::CStr _SettingsPath, int64 _RefreshInterval, NStr::CStr _Shell)
			-> NConcurrency::TCFuture<uint32>
		;
	};
}
