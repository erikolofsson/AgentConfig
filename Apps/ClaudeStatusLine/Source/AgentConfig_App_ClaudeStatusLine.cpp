// Copyright © Erik Olofsson

#include <Mib/Core/Core>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>

#include "AgentConfig_App_ClaudeStatusLine.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	CClaudeStatusLineActor::CClaudeStatusLineActor()
		: CDistributedToolAppActor
		(
			NConcurrency::CDistributedToolSettings{"ClaudeStatusLine"}
			.f_TLSForLocalSockets(false)
			.f_InProcessCommandLineOnly(true)
			.f_TimeoutForUnixSockets(false)
			.f_DefaultCommandLineFunctionalies
			(
				NConcurrency::EDefaultCommandLineFunctionality
				(
					// Terminal keeps --color and the width overrides, and measured free. The rest
					// registers commands this tool has no use for: distributed computing and
					// authentication trust management, the sensor store, and the distributed log
					// reader that the compiled out log system already removes
					NConcurrency::EDefaultCommandLineFunctionality_Help
					| NConcurrency::EDefaultCommandLineFunctionality_Terminal
				)
			)
		)
	{
	}

	CClaudeStatusLineActor::~CClaudeStatusLineActor()
	{
	}
}
