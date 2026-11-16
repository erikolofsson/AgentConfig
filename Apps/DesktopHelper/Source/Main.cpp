// Copyright © Erik Olofsson

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>

#include "AgentConfig_App_DesktopHelper.h"

using namespace NMib;
using namespace NMib::NAgentConfig::NDesktopHelper;

struct CDesktopHelper : public CApplication
{
private:
	aint f_Main();
};

aint CDesktopHelper::f_Main()
{
	NConcurrency::CDistributedDaemon Daemon
		{
			"AgentConfigDesktopHelper"
			, "Desktop Helper"
			, "Runs commands for agents in the desktop session"
			, []
			{
				return fg_ConstructActor<CDesktopHelperActor>();
			}
		}
	;

	// The helper is only useful inside the desktop session, and a user daemon starts at login there
	Daemon.m_DefaultMode = NDaemon::EDaemonMode_LocalUser;

#ifdef DPlatformFamily_macOS
	// Makes the launch agent ProcessType Interactive. Without it launchd spawns it as a standard daemon, which with the processes
	// it starts runs at the utility priority of 20 instead of the 31 of a shell, with the CPU frequency held down
	Daemon.m_ExecutionPriority = EExecutionPriority_High;
#endif

	return Daemon.f_Run();
}

DAppImplement(CDesktopHelper);
