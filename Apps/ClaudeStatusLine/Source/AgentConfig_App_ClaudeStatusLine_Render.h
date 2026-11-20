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

	// The variables of Claude Code's environment that move where autocompact starts
	struct CAutocompactSettings
	{
		fp64 m_PctOverride = 0.0;    // CLAUDE_AUTOCOMPACT_PCT_OVERRIDE, 0 when unset
		int64 m_MaxOutputTokens = 0; // CLAUDE_CODE_MAX_OUTPUT_TOKENS, 0 when unset
	};

	struct CRenderResult
	{
		NStr::CStr m_Line;
		// The cache entries the render found stale, "claude" and/or "codex"
		NContainer::TCVector<NStr::CStr> m_StaleTargets;
	};

	NStorage::TCOptional<int64> fg_ParseClaudeCodeInteger(NStr::CStr const &_Value);
	int64 fg_AutocompactReserveTenths(fp64 _ContextWindowSize, CAutocompactSettings const &_Settings);
	bool fg_ScopedLimitApplies(NStr::CStr const &_ModelName, CUsageLimit const &_Scoped);
	CRenderResult fg_RenderStatusLine(CStatusLineInput const &_Input, CStatusCache const &_Cache, int64 _Now, CAutocompactSettings const &_Autocompact);
}
