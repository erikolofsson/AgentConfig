// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Encoding/Json>

#include "AgentConfig_App_ClaudeStatusLine_Cache.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	// The fields of the status document Claude Code writes on stdin that the line is built from
	struct CStatusLineInput
	{
		static CStatusLineInput fs_FromJson(NEncoding::CJsonSorted const &_Json);

		// The weekly value as text, what the refresh child records as the value it has seen
		NStr::CStr m_WeeklyText;
		// The model in use, as Claude Code displays it
		NStr::CStr m_ModelName;
		NStorage::TCOptional<int64> m_WeeklyResetsAt;
		fp64 m_UsedPercentage = 0.0;
		fp64 m_ContextWindowSize = 0.0;
		fp64 m_WeeklyPercent = 0.0;
		bool m_bWeeklyPresent = false;
	};

	struct CRenderResult
	{
		NStr::CStr m_Line;
		// The cache entries the render found stale, "claude" and/or "codex"
		NContainer::TCVector<NStr::CStr> m_StaleTargets;
	};

	CRenderResult fg_RenderStatusLine(CStatusLineInput const &_Input, CStatusCache const &_Cache, int64 _Now, fp64 _AutocompactOverride);
}
