// Copyright © Erik Olofsson

#include <Mib/Core/Core>
#include <Mib/Encoding/JsonShortcuts>

#include "AgentConfig_App_ClaudeStatusLine.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	void CClaudeStatusLineActor::fp_BuildCommandLine(NConcurrency::CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		// Lets the base class create the "Tools" section and register every CDistributedTool implementation.
		CDistributedToolAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Claude Code status line"
				, "Renders the Claude Code status line from the status JSON document supplied on stdin."
			)
		;
	}
}
