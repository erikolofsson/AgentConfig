// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	enum class EStatusLineShell
	{
		mc_Bash
		, mc_PowerShell
	};

	NStr::CStr fg_NormalizedPath(NStr::CStr const &_Path);
	EStatusLineShell fg_StatusLineShell();
	NStr::CStr fg_StatusLineCommand(NStr::CStr const &_ProgramPath, EStatusLineShell _Shell);
}
