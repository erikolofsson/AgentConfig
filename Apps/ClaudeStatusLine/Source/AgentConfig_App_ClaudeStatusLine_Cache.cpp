// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/OnScopeExitCatch>
#include <Mib/Cryptography/RandomID>
#include <Mib/File/File>
#include <Mib/String/UnicodeConversion>
#include <Mib/Time/Time>

#include "AgentConfig_App_ClaudeStatusLine_Cache.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	using namespace NMib::NStr;

	namespace
	{
		bool fg_IsDigit(ch8 _Char)
		{
			return _Char >= '0' && _Char <= '9';
		}

		bool fg_IsAllDigits(CStr const &_Str)
		{
			if (_Str.f_IsEmpty())
				return false;

			for (ch8 const *pChar = _Str.f_GetStr(); *pChar; ++pChar)
			{
				if (!fg_IsDigit(*pChar))
					return false;
			}

			return true;
		}

		// Reads exactly _nDigits digits at _pStr, advancing it past them
		bool fg_ReadDigits(ch8 const * &_pStr, aint _nDigits, int64 &o_Value)
		{
			int64 Value = 0;

			for (aint iDigit = 0; iDigit < _nDigits; ++iDigit)
			{
				if (!fg_IsDigit(_pStr[iDigit]))
					return false;

				Value = Value * 10 + (_pStr[iDigit] - '0');
			}

			_pStr += _nDigits;
			o_Value = Value;

			return true;
		}

		// The ISO-8601 forms Date.parse accepts: a date, optionally followed by a time with an
		// optional fraction and an optional zone. A date alone is UTC, a time without a zone is local
		NStorage::TCOptional<int64> fg_ParseIso8601(CStr const &_Str)
		{
			ch8 const *pStr = _Str.f_GetStr();
			int64 Year;
			int64 Month;
			int64 Day;
			int64 Hour = 0;
			int64 Minute = 0;
			int64 Second = 0;
			int64 Offset = 0;
			bool bHasTime = false;
			bool bHasZone = false;

			if (!fg_ReadDigits(pStr, 4, Year) || *pStr != '-')
				return {};

			++pStr;
			if (!fg_ReadDigits(pStr, 2, Month) || *pStr != '-')
				return {};

			++pStr;
			if (!fg_ReadDigits(pStr, 2, Day))
				return {};

			if (*pStr == 'T' || *pStr == ' ')
			{
				++pStr;
				bHasTime = true;
				if (!fg_ReadDigits(pStr, 2, Hour) || *pStr != ':')
					return {};

				++pStr;
				if (!fg_ReadDigits(pStr, 2, Minute))
					return {};

				if (*pStr == ':')
				{
					++pStr;
					if (!fg_ReadDigits(pStr, 2, Second))
						return {};

					if (*pStr == '.' || *pStr == ',')
					{
						++pStr;
						if (!fg_IsDigit(*pStr))
							return {};

						// The fraction is dropped, the epoch is whole seconds
						while (fg_IsDigit(*pStr))
							++pStr;
					}
				}

				if (*pStr == 'Z')
				{
					++pStr;
					bHasZone = true;
				}
				else if (*pStr == '+' || *pStr == '-')
				{
					int64 Sign = *pStr == '-' ? -1 : 1;
					int64 OffsetHours;
					int64 OffsetMinutes = 0;

					++pStr;
					if (!fg_ReadDigits(pStr, 2, OffsetHours))
						return {};

					if (*pStr == ':')
						++pStr;

					if (fg_IsDigit(*pStr) && !fg_ReadDigits(pStr, 2, OffsetMinutes))
						return {};

					Offset = Sign * (OffsetHours * 3600 + OffsetMinutes * 60);
					bHasZone = true;
				}
			}

			if (*pStr)
				return {};

			if (Month < 1 || Month > 12 || Day < 1 || Day > 31 || Hour > 24 || Minute > 59 || Second > 59)
				return {};

			NTime::CTime Time = NTime::CTimeConvert::fs_CreateTime(Year, aint(Month), aint(Day), aint(Hour), aint(Minute), aint(Second));

			if (bHasTime && !bHasZone)
				Time = Time.f_ToUTC();

			return int64(NTime::CTimeConvert(Time).f_UnixSeconds()) - Offset;
		}

		NStorage::TCOptional<int64> fg_EpochFromNumber(fp64 _Value)
		{
			if (_Value != _Value)
				return {};

			if (_Value > 1e12)
				return (_Value / 1000.0).f_Floor().f_ToInt();

			return _Value.f_Floor().f_ToInt();
		}

		// Integral values are written as integers, the way JSON.stringify writes them
		void fg_AssignNumber(NEncoding::CJsonSorted &o_Value, fp64 _Number)
		{
			if (_Number == _Number.f_Floor() && fg_Abs(_Number) < 1e15)
				o_Value = _Number.f_ToInt();
			else
				o_Value = _Number;
		}

		// Every session shares the cache, so claiming a refresh and writing results are serialised
		// through a lock file beside it. Readers need no lock, the cache is replaced atomically
		struct CCacheLock
		{
			CCacheLock();

			NFile::CLockFile m_Lock;
		};

		CCacheLock::CCacheLock()
		{
			CStr Path = fg_GetCachePath();

			NFile::CFile::fs_CreateDirectory(NFile::CFile::fs_GetPath(Path));
			m_Lock.f_SetLockFile(Path + ".lock");
			// Every holder keeps it for about a millisecond, so waiting is for the rare case of
			// sessions claiming at the same moment rather than for anything slow
			m_Lock.f_LockWithException(30.0);
		}

		CStr fg_MemberText(NEncoding::CJsonSorted const *_pParent, ch8 const *_pName)
		{
			NEncoding::CJsonSorted const *pMember = fg_JsonMember(_pParent, _pName);
			if (!pMember || !pMember->f_IsString())
				return {};

			return pMember->f_String();
		}
	}

	CStatusCache CStatusCache::fs_FromJson(NEncoding::CJsonSorted const &_Json)
	{
		CStatusCache Return;

		if (NEncoding::CJsonSorted const *pClaude = _Json.f_GetMember("claude", NEncoding::EJsonType_Object))
		{
			CClaudeCache Claude;

			Claude.m_FetchedAt = fg_JsonNumber(fg_JsonMember(pClaude, "fetchedAt")).f_ToInt();
			Claude.m_FetchedAtMs = fg_JsonNumber(fg_JsonMember(pClaude, "fetchedAtMs")).f_ToInt();
			Claude.m_AttemptedAt = fg_JsonNumber(fg_JsonMember(pClaude, "attemptedAt")).f_ToInt();
			Claude.m_SeenWeekly = fg_MemberText(pClaude, "seenWeekly");
			Claude.m_LastError = fg_MemberText(pClaude, "lastError");
			Claude.m_ClaimID = fg_MemberText(pClaude, "claimId");

			auto fReadLimit = [](NEncoding::CJsonSorted const *_pParent, ch8 const *_pName) -> NStorage::TCOptional<CUsageLimit>
				{
					NEncoding::CJsonSorted const *pLimit = _pParent->f_GetMember(_pName, NEncoding::EJsonType_Object);

					// A limit without a percent counts as no limit, the way the script reads it
					if (!pLimit || !pLimit->f_GetMember("percent"))
						return {};

					CUsageLimit Limit;

					Limit.m_Name = fg_MemberText(pLimit, "name");
					Limit.m_Percent = fg_JsonNumber(fg_JsonMember(pLimit, "percent"));
					Limit.m_ResetsAt = fg_JsonNumber(fg_JsonMember(pLimit, "resetsAt")).f_ToInt();

					return Limit;
				}
			;

			Claude.m_Scoped = fReadLimit(pClaude, "scoped");
			Claude.m_AllModels = fReadLimit(pClaude, "all");

			if (NEncoding::CJsonSorted const *pSpend = pClaude->f_GetMember("spend", NEncoding::EJsonType_Object))
			{
				CSpendLimit Spend;

				Spend.m_Currency = fg_MemberText(pSpend, "currency");
				Spend.m_UsedMinor = fg_JsonNumber(fg_JsonMember(pSpend, "used")).f_ToInt();
				Spend.m_Exponent = fg_JsonNumber(fg_JsonMember(pSpend, "exponent")).f_ToInt();

				if (NEncoding::CJsonSorted const *pLimit = fg_JsonMember(pSpend, "limit"))
					Spend.m_LimitMinor = fg_JsonNumber(pLimit).f_ToInt();

				Claude.m_Spend = Spend;
			}

			Return.m_Claude = Claude;
		}

		if (NEncoding::CJsonSorted const *pCodex = _Json.f_GetMember("codex", NEncoding::EJsonType_Object))
		{
			CCodexCache Codex;

			Codex.m_Percent = fg_JsonNumber(fg_JsonMember(pCodex, "percent"));
			Codex.m_ResetsAt = fg_JsonNumber(fg_JsonMember(pCodex, "resetsAt")).f_ToInt();
			Codex.m_WindowSeconds = fg_JsonNumber(fg_JsonMember(pCodex, "windowSeconds")).f_ToInt();

			if (NEncoding::CJsonSorted const *pCredits = fg_JsonMember(pCodex, "credits"))
				Codex.m_Credits = fg_JsonNumber(pCredits);

			Codex.m_FetchedAt = fg_JsonNumber(fg_JsonMember(pCodex, "fetchedAt")).f_ToInt();
			Codex.m_SnapshotAt = fg_JsonNumber(fg_JsonMember(pCodex, "snapshotAt")).f_ToInt();
			Codex.m_AttemptedAt = fg_JsonNumber(fg_JsonMember(pCodex, "attemptedAt")).f_ToInt();

			Return.m_Codex = Codex;
		}

		return Return;
	}

	NEncoding::CJsonSorted CStatusCache::f_ToJson() const
	{
		NEncoding::CJsonSorted Json(NEncoding::EJsonType_Object);

		if (m_Claude)
		{
			NEncoding::CJsonSorted &Claude = Json["claude"];

			Claude = NEncoding::EJsonType_Object;
			Claude["fetchedAt"] = m_Claude->m_FetchedAt;

			if (m_Claude->m_FetchedAtMs)
				Claude["fetchedAtMs"] = m_Claude->m_FetchedAtMs;

			Claude["seenWeekly"] = m_Claude->m_SeenWeekly;

			if (m_Claude->m_AttemptedAt)
				Claude["attemptedAt"] = m_Claude->m_AttemptedAt;

			if (!m_Claude->m_LastError.f_IsEmpty())
				Claude["lastError"] = m_Claude->m_LastError;

			if (!m_Claude->m_ClaimID.f_IsEmpty())
				Claude["claimId"] = m_Claude->m_ClaimID;

			auto fWriteLimit = [](NEncoding::CJsonSorted &o_Parent, ch8 const *_pName, CUsageLimit const &_Limit)
				{
					NEncoding::CJsonSorted &Limit = o_Parent[_pName];

					Limit = NEncoding::EJsonType_Object;

					if (!_Limit.m_Name.f_IsEmpty())
						Limit["name"] = _Limit.m_Name;

					fg_AssignNumber(Limit["percent"], _Limit.m_Percent);
					Limit["resetsAt"] = _Limit.m_ResetsAt;
				}
			;

			if (m_Claude->m_Scoped)
				fWriteLimit(Claude, "scoped", *m_Claude->m_Scoped);

			if (m_Claude->m_AllModels)
				fWriteLimit(Claude, "all", *m_Claude->m_AllModels);

			if (m_Claude->m_Spend)
			{
				NEncoding::CJsonSorted &Spend = Claude["spend"];

				Spend = NEncoding::EJsonType_Object;
				Spend["currency"] = m_Claude->m_Spend->m_Currency;
				Spend["used"] = m_Claude->m_Spend->m_UsedMinor;
				Spend["exponent"] = m_Claude->m_Spend->m_Exponent;

				if (m_Claude->m_Spend->m_LimitMinor)
					Spend["limit"] = *m_Claude->m_Spend->m_LimitMinor;
			}
		}

		if (m_Codex)
		{
			NEncoding::CJsonSorted &Codex = Json["codex"];

			Codex = NEncoding::EJsonType_Object;
			fg_AssignNumber(Codex["percent"], m_Codex->m_Percent);
			Codex["resetsAt"] = m_Codex->m_ResetsAt;
			Codex["windowSeconds"] = m_Codex->m_WindowSeconds;

			if (m_Codex->m_Credits)
				fg_AssignNumber(Codex["credits"], *m_Codex->m_Credits);

			Codex["fetchedAt"] = m_Codex->m_FetchedAt;
			Codex["snapshotAt"] = m_Codex->m_SnapshotAt;

			if (m_Codex->m_AttemptedAt)
				Codex["attemptedAt"] = m_Codex->m_AttemptedAt;
		}

		return Json;
	}

	CStr fg_GetHomeDirectory()
	{
		CStr Home = fg_GetSys()->f_GetEnvironmentVariable("HOME");

		if (Home.f_IsEmpty())
			Home = NSys::NFile::fg_GetUserHomeDirectory();

		return Home;
	}

	CStr fg_GetCachePath()
	{
		return fg_GetHomeDirectory() / ".claude/statusline-cache.json";
	}

	CStatusCache fg_ReadCache()
	{
		try
		{
			return CStatusCache::fs_FromJson(NEncoding::CJsonSorted::fs_FromString(NFile::CFile::fs_ReadStringFromFile(fg_GetCachePath(), true)));
		}
		catch (NException::CException const &)
		{
			return {};
		}
	}

	void fg_WriteCache(CStatusCache const &_Cache)
	{
		CStr Path = fg_GetCachePath();
		CStr TemporaryPath = "{}.tmp.{}"_f << Path << NCryptography::fg_FastRandomID();

		NFile::CFile::fs_WriteStringToFile(TemporaryPath, _Cache.f_ToJson().f_ToString("  "), false);
		NFile::CFile::fs_AtomicReplaceFile(TemporaryPath, Path);
	}

	namespace
	{
		// When the usage was fetched, in milliseconds, for entries written before fetchedAtMs from their seconds
		int64 fg_FetchOrder(CClaudeCache const &_Claude)
		{
			return _Claude.m_FetchedAtMs ? _Claude.m_FetchedAtMs : _Claude.m_FetchedAt * 1000;
		}
	}

	CRefreshClaim fg_ClaimRefresh(NContainer::TCVector<CStr> const &_Targets, int64 _Observed, int64 _Now, CStr const &_ClaimID)
	{
		CCacheLock Lock;
		CStatusCache Cache = fg_ReadCache();
		CRefreshClaim Claim;

		for (CStr const &Target : _Targets)
		{
			if (Target == "claude")
			{
				if (_Observed && Cache.m_Claude && fg_Max(Cache.m_Claude->m_FetchedAt, Cache.m_Claude->m_AttemptedAt) >= _Observed)
					continue;

				if (!Cache.m_Claude)
					Cache.m_Claude = CClaudeCache{};

				Cache.m_Claude->m_AttemptedAt = _Now;
				Cache.m_Claude->m_ClaimID = _ClaimID;
				Claim.m_ClaudeFetchOrder = fg_FetchOrder(*Cache.m_Claude);
				// The error of an earlier claim would otherwise be taken for this one's when other usage is saved over it
				Cache.m_Claude->m_LastError.f_Clear();
			}
			else if (Target == "codex")
			{
				if (_Observed && Cache.m_Codex && fg_Max(Cache.m_Codex->m_FetchedAt, Cache.m_Codex->m_AttemptedAt) >= _Observed)
					continue;

				if (!Cache.m_Codex)
					Cache.m_Codex = CCodexCache{};

				Cache.m_Codex->m_AttemptedAt = _Now;
			}
			else
				continue;

			Claim.m_Targets.f_Insert(Target);
		}

		if (!Claim.m_Targets.f_IsEmpty())
			fg_WriteCache(Cache);

		return Claim;
	}

	void fg_SaveRefreshed
		(
			NStorage::TCOptional<CClaudeCache> const &_Claude
			, NStr::CStr const &_ClaudeError
			, NStorage::TCOptional<CCodexCache> const &_Codex
			, NStr::CStr const &_ClaimID
			, int64 _ClaudeFetchOrderAtClaim
		)
	{
		CCacheLock Lock;
		CStatusCache Cache = fg_ReadCache();

		fg_ApplyClaudeRefresh(Cache, _Claude, _ClaudeError, _ClaimID, _ClaudeFetchOrderAtClaim);

		if (_Codex)
			Cache.m_Codex = *_Codex;

		fg_WriteCache(Cache);
	}

	void fg_ApplyClaudeRefresh(CStatusCache &o_Cache, NStorage::TCOptional<CClaudeCache> const &_Claude, CStr const &_ClaudeError, CStr const &_ClaimID, int64 _ClaudeFetchOrderAtClaim)
	{
		bool bOwnClaim = o_Cache.m_Claude && o_Cache.m_Claude->m_ClaimID == _ClaimID;

		if (_Claude)
		{
			bool bSavedSinceClaim = o_Cache.m_Claude && fg_FetchOrder(*o_Cache.m_Claude) != _ClaudeFetchOrderAtClaim;

			if (bSavedSinceClaim && fg_FetchOrder(*o_Cache.m_Claude) > fg_FetchOrder(*_Claude))
			{
				// Usage fetched later stays. The refresh that owns the claim has still finished, and without an error
				if (bOwnClaim)
				{
					o_Cache.m_Claude->m_ClaimID.f_Clear();
					o_Cache.m_Claude->m_LastError.f_Clear();
				}

				return;
			}

			if (bOwnClaim || !o_Cache.m_Claude)
			{
				o_Cache.m_Claude = *_Claude;
				return;
			}

			CClaudeCache Claude = *_Claude;

			Claude.m_ClaimID = o_Cache.m_Claude->m_ClaimID;
			Claude.m_AttemptedAt = o_Cache.m_Claude->m_AttemptedAt;
			Claude.m_LastError = o_Cache.m_Claude->m_LastError;
			o_Cache.m_Claude = fg_Move(Claude);
		}
		else if (!_ClaudeError.f_IsEmpty() && bOwnClaim)
			o_Cache.m_Claude->m_LastError = _ClaudeError;
	}

	CStr fg_CacheErrorText(CStr const &_Error, umint _MaxBytes)
	{
		CStr Text = NStr::fg_IsValidUTF8(_Error) ? _Error : NStr::fg_ReplaceSequenceUTF8(_Error);

		if (Text.f_GetLen() <= _MaxBytes)
			return Text;

		NStr::CStrIteratorUTF8 iCharacter(Text.f_GetStr(), _MaxBytes);

		while (iCharacter && iCharacter.f_IsWholeCodePoint())
			++iCharacter;

		return Text.f_Left(iCharacter.f_GetLastWholeCodePointPos()) + "...";
	}

	int64 fg_UnixNowMilliseconds()
	{
		return int64(NTime::CTimeConvert(NTime::CTime::fs_NowUTC()).f_UnixMilliseconds());
	}

	int64 fg_UnixNow()
	{
		NTime::CTime Now = NTime::CTime::fs_NowUTC();

		return int64(NTime::CTimeConvert(Now).f_UnixSeconds());
	}

	NEncoding::CJsonSorted const *fg_JsonMember(NEncoding::CJsonSorted const *_pParent, ch8 const *_pName)
	{
		if (!_pParent || !_pParent->f_IsObject())
			return nullptr;

		NEncoding::CJsonSorted const *pMember = _pParent->f_GetMember(_pName);
		if (!pMember || pMember->f_IsNull())
			return nullptr;

		return pMember;
	}

	fp64 fg_JsonNumber(NEncoding::CJsonSorted const *_pValue)
	{
		if (!_pValue)
			return 0.0;

		return _pValue->f_AsFloat(0.0);
	}

	CStr fg_JsonText(NEncoding::CJsonSorted const *_pValue)
	{
		if (!_pValue)
			return {};

		return _pValue->f_AsString();
	}

	NStorage::TCOptional<int64> fg_ToEpoch(NEncoding::CJsonSorted const *_pValue)
	{
		if (!_pValue || _pValue->f_IsNull())
			return {};

		if (_pValue->f_IsInteger())
			return fg_EpochFromNumber(fp64(_pValue->f_Integer()));

		if (_pValue->f_IsFloat())
			return fg_EpochFromNumber(_pValue->f_Float());

		CStr Text = fg_JsonText(_pValue);

		if (Text.f_IsEmpty())
			return {};

		if (fg_IsAllDigits(Text))
		{
			int64 Value = Text.f_ToIntExact(int64(0));

			if (Text.f_GetLen() >= 13)
				return Value / 1000;

			return Value;
		}

		return fg_ParseIso8601(Text);
	}
}
