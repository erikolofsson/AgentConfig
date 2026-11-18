// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/File/File>

#include "AgentConfig_App_DesktopHelper.h"

namespace NMib::NAgentConfig::NDesktopHelper
{
	namespace
	{
		NStr::CStr fg_NotRunningMessage()
		{
			using namespace NStr;

			return
				"The desktop helper is not running, so there is no desktop session to run the command in.\n"
				"Ask the user to start it from a terminal in their desktop session:\n"
				"\n"
				"    {0}\n"
				"\n"
				"or to install it as a user daemon that starts when they log in:\n"
				"\n"
				"    {0} --daemon-add"_f
				<< NFile::CFile::fs_GetProgramPath()
			;
		}

		// A helper belongs to the desktop session of one user, so its socket and trust databases live in that user's home
		// directory rather than next to an executable that several users can share
		NConcurrency::CDistributedAppActor_Settings fg_DesktopHelperSettings()
		{
			return NConcurrency::CDistributedAppActor_Settings{"DesktopHelper"}
				.f_RootDirectory(NSys::NFile::fg_GetUserHomeDirectory() / ".AgentConfig/DesktopHelper")
				.f_TLSForLocalSockets(false)
				.f_WaitForRemotes(false)
				.f_NotRunningMessage(fg_NotRunningMessage())
				.f_DefaultCommandLineFunctionalies
				(
					NConcurrency::EDefaultCommandLineFunctionality
					(
						NConcurrency::EDefaultCommandLineFunctionality_Help | NConcurrency::EDefaultCommandLineFunctionality_Logging | NConcurrency::EDefaultCommandLineFunctionality_Terminal
					)
				)
			;
		}
	}

	CDesktopHelperActor::CDesktopHelperActor()
		: CDistributedAppActor(fg_DesktopHelperSettings())
	{
	}

	CDesktopHelperActor::~CDesktopHelperActor()
	{
	}

	NConcurrency::TCFuture<void> CDesktopHelperActor::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		co_return {};
	}

	NConcurrency::TCFuture<void> CDesktopHelperActor::fp_StopApp()
	{
		co_return {};
	}
}
