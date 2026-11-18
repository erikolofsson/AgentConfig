// Copyright © Erik Olofsson

#pragma once

#include <Mib/Core/Core>
#include <Mib/Encoding/Json>
#include <Mib/Storage/Optional>

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	// A weekly limit from api.anthropic.com/api/oauth/usage
	struct CUsageLimit
	{
		// The model the limit is scoped to, empty for the one that covers every model
		NStr::CStr m_Name;
		fp64 m_Percent = 0.0;
		int64 m_ResetsAt = 0;
	};

	struct CClaudeCache
	{
		NStorage::TCOptional<CUsageLimit> m_Scoped;
		// The limit covering every model, the same one the status document reports as
		// rate_limits.seven_day. Kept because a document does not carry it until the session has
		// made a request, and the line would otherwise have nothing to show at start-up
		NStorage::TCOptional<CUsageLimit> m_AllModels;
		NStr::CStr m_SeenWeekly;
		int64 m_FetchedAt = 0;
		// When a refresh was last started that has not (yet) replaced this entry. It ages the entry
		// the same way m_FetchedAt does, so a refresh that is still running, or that failed, is
		// not started again on every render
		int64 m_AttemptedAt = 0;
	};

	struct CCodexCache
	{
		fp64 m_Percent = 0.0;
		int64 m_ResetsAt = 0;
		int64 m_WindowSeconds = 0;
		NStorage::TCOptional<fp64> m_Credits; // The credit balance, none when the account has none or they are unlimited
		int64 m_FetchedAt = 0;
		int64 m_SnapshotAt = 0;
		int64 m_AttemptedAt = 0;
	};

	// Everything a render needs beyond the document on stdin, kept in one file so a render is a
	// single read. Network and process spawning only ever happen in the detached refresh child
	struct CStatusCache
	{
		static CStatusCache fs_FromJson(NEncoding::CJsonSorted const &_Json);

		NEncoding::CJsonSorted f_ToJson() const;

		NStorage::TCOptional<CClaudeCache> m_Claude;
		NStorage::TCOptional<CCodexCache> m_Codex;
	};

	NStr::CStr fg_GetHomeDirectory();
	NStr::CStr fg_GetCachePath();

	// An empty cache when the file is missing or unreadable, the render degrades rather than fails
	CStatusCache fg_ReadCache();
	// Written to a sibling temporary file and renamed into place, so a render never sees a partial file
	void fg_WriteCache(CStatusCache const &_Cache);
	// Claims entries ("claude", "codex") for a refresh under the cache's lock and returns the ones
	// claimed, each stamped with _Now so nothing else refreshes them again for an interval. An
	// entry that anything touched at or after _Observed belongs to whoever touched it and is left
	// alone, which is how sessions sharing the cache end up doing the work once. An _Observed of 0
	// claims whatever is asked for
	NContainer::TCVector<NStr::CStr> fg_ClaimRefresh(NContainer::TCVector<NStr::CStr> const &_Targets, int64 _Observed, int64 _Now);

	// Writes the entries a refresh fetched into the cache under its lock, keeping whatever another
	// session wrote in the meantime for the entries this one did not fetch
	void fg_SaveRefreshed(NStorage::TCOptional<CClaudeCache> const &_Claude, NStorage::TCOptional<CCodexCache> const &_Codex);

	int64 fg_UnixNow();

	// A member that exists and is not null, the way `a?.b ?? c` treats members
	NEncoding::CJsonSorted const *fg_JsonMember(NEncoding::CJsonSorted const *_pParent, ch8 const *_pName);

	// The value as a number, 0 when there is none or it is not one
	fp64 fg_JsonNumber(NEncoding::CJsonSorted const *_pValue);
	// The value as text, empty when there is none
	NStr::CStr fg_JsonText(NEncoding::CJsonSorted const *_pValue);
	// Accepts ISO-8601, epoch seconds, or epoch milliseconds, as a number or a string
	NStorage::TCOptional<int64> fg_ToEpoch(NEncoding::CJsonSorted const *_pValue);
}
