// Copyright © Erik Olofsson

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedApp>

#include "AgentConfig_App_ClaudeStatusLine.h"

using namespace NMib;
using namespace NMib::NAgentConfig::NClaudeStatusLine;

struct CClaudeStatusLine : public CApplication
{
private:
	aint f_Main();
};

aint CClaudeStatusLine::f_Main()
{
	// The status line lives for a few milliseconds and its whole run is on the actor pool, so
	// the pool is user initiated rather than default: a thread that short never gets to show
	// the scheduler what it needs, and the band says it up front. Set before the concurrency
	// manager exists, since its pool threads take their band when they start
	NMib::NSys::fg_Thread_SetPriority(NMib::NSys::fg_Thread_GetCurrent(), NMib::EExecutionPriority_AboveNormal);
	NConcurrency::fg_SetConcurrencyManagerDefaultExecutionPriority(NConcurrency::EPriority_NormalHighCPU, EExecutionPriority_AboveNormal);
	NConcurrency::fg_SetConcurrencyManagerDefaultExecutionPriority(NConcurrency::EPriority_Normal, EExecutionPriority_AboveNormal);

	// The distribution manager's resolve actor declares the high CPU band, which costs a second pool
	// thread for a lookup this tool never makes
	NConcurrency::fg_SetConcurrencyManagerDefaultPriorityClamp(NConcurrency::EPriority_Normal);

	// These are short lived command line tools, so every extra worker thread is startup and
	// shutdown cost that is never repaid by parallelism.
	NConcurrency::fg_SetConcurrencyManagerDefaultThreadCount(1);

	return NConcurrency::fg_RunApp
		(
			[]
			{
				return fg_ConstructActor<CClaudeStatusLineActor>();
			}
		)
	;
}

DAppImplement(CClaudeStatusLine);
