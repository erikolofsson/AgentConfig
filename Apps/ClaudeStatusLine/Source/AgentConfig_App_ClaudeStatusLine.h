// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Concurrency/DistributedTool>

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	// The application actor. It is a tool app, meaning every CDistributedTool implementation linked into
	// the executable is instantiated and gets to register its own commands.
	struct CClaudeStatusLineActor : public NConcurrency::CDistributedToolAppActor
	{
		CClaudeStatusLineActor();
		~CClaudeStatusLineActor();

	private:
		void fp_BuildCommandLine(NConcurrency::CDistributedAppCommandLineSpecification &o_CommandLine) override;
	};
}
