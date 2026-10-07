// Copyright © Erik Olofsson

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>

#include "AgentConfig_App_DesktopHelper.h"

using namespace NMib;
using namespace NMib::NAgentConfig::NDesktopHelper;

class CDesktopHelper : public CApplication
{
	aint f_Main()
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

		return Daemon.f_Run();
	}
};

DAppImplement(CDesktopHelper);
