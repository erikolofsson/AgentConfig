// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/File/File>

#include "AgentConfig_App_DesktopHelper.h"

namespace NMib::NAgentConfig::NDesktopHelper
{
	using namespace NMib::NStr;
	using namespace NMib::NConcurrency;

	namespace
	{
		NContainer::TCVector<CStr> fg_EnvironmentNames(NEncoding::CEJsonSorted const &_Params)
		{
			// Absent when --env is not given, so that a run without it keeps working with helpers and command lines from before it
			if (auto const *pEnvironment = _Params.f_GetMember("Environment"))
				return pEnvironment->f_StringArray();

			return {};
		}
	}

	void CDesktopHelperActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Desktop helper"
				, "Runs commands in the desktop session for agents that are connected some other way, such as over SSH.\n"
				"Start it from the desktop by running it without a command, or install it with --daemon-add to start it at login.\n"
			)
		;

		auto Section = o_CommandLine.f_AddSection("Desktop", "Commands that run in the desktop session.");

		Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--run"]
					, "Description"_o= "Run an executable in the desktop session and wait for it to exit. The command line is logged, so pass secrets with "
					"--env instead.\n"
					"Stdin, stdout and stderr are forwarded, and the exit status is the exit status of the executable.\n"
					"Put -- before the executable so that its arguments are not parsed as options: --run -- open -a Safari\n"
					"The audit log in ~/.AgentConfig/DesktopHelper/Log keeps every command line and deletes nothing, also variables set "
					"with /usr/bin/env NAME=VALUE. Pass secrets in variables with --env, on stdin or in a file the executable reads, never in the command "
					"line.\n"
					, "Options"_o=
					{
						"WorkingDirectory?"_o=
						{
							"Names"_o= _o["--working-directory"]
							, "Default"_o= ""
							, "Description"_o= "The working directory of the process, relative to the current directory of the command line, which is "
							"also the default.\n"
						}
						, "Environment?"_o=
						{
							"Names"_o= _o["--env"]
							, "Type"_o= _o[""]
							, "Description"_o= "Comma separated names of variables to copy from the environment of the command line to the process, "
							"such as --env GITHUB_TOKEN,NPM_TOKEN. They override the variables of the desktop session. The audit log gets only the "
							"names, which are letters, digits and _, and a variable that is not set fails the run. PATH, which would change the "
							"executable a bare name runs, is "
							"refused; set it with /usr/bin/env PATH=... instead.\n"
						}
						, "CallerDirectory?"_o=
						{
							"Names"_o= _o[]
							, "Default"_o= NFile::CFile::fs_GetCurrentDirectory()
							, "Hidden"_o= true
							, "Description"_o= "The current directory of the command line, which runs in a process of its own.\n"
						}
					}
					, "Parameters"_o=
					{
						"Executable"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The executable to run. A name without a directory is looked up in the PATH of the desktop session, "
							"and one with a directory is relative to the working directory.\n"
						}
						, "Arguments...?"_o=
						{
							"Type"_o= _o[""]
							, "Default"_o= _o[]
							, "Description"_o= "The arguments to the executable.\n"
						}
					}
				}
				, [this](NEncoding::CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine) -> TCFuture<uint32>
				{
					CStr WorkingDirectory = _Params["CallerDirectory"].f_String();
					if (CStr const &RequestedDirectory = _Params["WorkingDirectory"].f_String())
						WorkingDirectory = NFile::CFile::fs_GetExpandedPath(RequestedDirectory, WorkingDirectory);

					CStr Executable = _Params["Executable"].f_String();
					if (Executable.f_FindChars("/\\") >= 0)
						Executable = NFile::CFile::fs_GetExpandedPath(Executable, WorkingDirectory);

					NContainer::TCVector<CStr> Arguments;
					for (auto const &Argument : _Params["Arguments"].f_Array())
						Arguments.f_Insert(Argument.f_String());

					return fp_RunInDesktop(fg_Move(_pCommandLine), fg_Move(Executable), fg_Move(Arguments), fg_Move(WorkingDirectory), fg_EnvironmentNames(_Params));
				}
				, EDistributedAppCommandFlag_FailIfNotRunning
				// Runs in the command line's process, which only gives out the variables its own --env names
				, [](NEncoding::CEJsonSorted _Params, NEncoding::CEJsonSorted _Query) -> TCFuture<NEncoding::CEJsonSorted>
				{
					NContainer::TCVector<CStr> Allowed = fg_EnvironmentNames(_Params);
					NEncoding::CEJsonSorted Answer;
					auto &Environment = Answer["Environment"];

					Environment = NEncoding::EEJsonType_Object;

					for (CStr const &Name : _Query["Environment"].f_StringArray())
					{
						if (Allowed.f_Contains(Name) < 0)
							co_return DMibErrorInstance("The variable {} was not named with --env"_f << Name);

						bool bExists = false;
						CStr Value = fg_GetSys()->f_GetEnvironmentVariable(Name, {}, &bExists);

						if (!bExists)
							co_return DMibErrorInstance("The variable {} given with --env is not set"_f << Name);

						Environment[Name] = fg_Move(Value);
					}

					co_return fg_Move(Answer);
				}
			)
		;
	}

	// Running the executable without a command from a terminal in the desktop session starts the helper in the
	// foreground. CDistributedDaemon adds --daemon-run-debug after fp_BuildCommandLine, and not at all for a remote
	// command line
	TCFuture<void> CDesktopHelperActor::fp_FinalizeCommandLine(NStorage::TCSharedPointer<CDistributedAppCommandLineSpecification> _pCommandLine)
	{
		if (auto RunDebugCommand = _pCommandLine->f_GetCommand("--daemon-run-debug"))
			_pCommandLine->f_SetDefaultCommand(*RunDebugCommand);

		co_return {};
	}
}
