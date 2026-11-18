// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "AgentConfig_App_ClaudeStatusLine_Cache.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	// The per-model weekly limit from api.anthropic.com/api/oauth/usage, using the OAuth token
	// from the macOS Keychain (or ~/.claude/.credentials.json). Fails with the reason when there
	// is no token or the request does not succeed
	NConcurrency::TCFuture<CClaudeCache> fg_FetchClaudeUsage(NStr::CStr _SeenWeekly);

	// The Codex weekly limit: `codex app-server` -> account/rateLimits/read, the same live call
	// Codex's own /status makes; it starts no session and burns no quota. Falls back to the last
	// snapshot in the newest session rollout, and fails with both reasons when that is missing too
	NConcurrency::TCFuture<CCodexCache> fg_FetchCodexUsage();

	// The success or failure of a result without its value, for reporting several at once
	NConcurrency::TCAsyncResult<void> fg_Outcome(NConcurrency::CAsyncResult const &_Result);
}
