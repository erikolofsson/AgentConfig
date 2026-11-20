// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/String/Appender>
#include <Mib/String/UnicodeConversion>
#include <Mib/Time/Time>
#include <Mib/Time/TimeSpan>

#include <cmath>
#include <cstdlib>

#include "AgentConfig_App_ClaudeStatusLine_Render.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	using namespace NMib::NStr;

	namespace
	{
		constexpr int64 gc_ClaudeMinRefresh = 60; // never hit the usage API more often than this
		constexpr int64 gc_ClaudeMaxAge = 600; // ...and refresh at least this often
		constexpr int64 gc_CodexMaxAge = 180; // spawning the codex binary costs ~0.5s
		constexpr int64 gc_CodexStaleAfter = 43200; // 12h: mark a fallback snapshot as stale
		constexpr int64 gc_ClaudeStaleAfter = 3600; // 1h of failed refreshes: mark the Claude usage from the cache as stale
		constexpr int64 gc_OutputReserveTokens = 20000; // What Claude Code keeps for output when the maximum output is larger
		constexpr int64 gc_SummaryBufferTokens = 13000; // What Claude Code keeps below the effective window for the summary
		constexpr int64 gc_DefaultContextWindowTokens = 200000;
		constexpr int64 gc_Week = 604800;
		constexpr aint gc_nContextBarChars = 12;
		constexpr aint gc_nBurnBarChars = 12;

		// Compile-time strings, so the appends know their lengths
		constexpr CStr gc_Blocks[] = {gc_Str<"█">, gc_Str<"▉">, gc_Str<"▊">, gc_Str<"▋">, gc_Str<"▌">, gc_Str<"▍">, gc_Str<"▎">, gc_Str<"▏">};
		constexpr CStr gc_Space = gc_Str<" ">;
		// Indexed by the halves of a cell filled from the left, in its top and its bottom row. The
		// full cell is the upper half block, for a top and bottom in different colours
		constexpr CStr gc_Quadrants[3][3] =
			{
				{gc_Str<" ">, gc_Str<"▖">, gc_Str<"▄">}
				, {gc_Str<"▘">, gc_Str<"▌">, gc_Str<"▙">}
				, {gc_Str<"▀">, gc_Str<"▛">, gc_Str<"▀">}
			}
		;
		constexpr CStr gc_DefaultModel = gc_Str<"M">;
		// The limit covering every model, while the model in use has a scoped limit of its own: what Opus draws from
		constexpr CStr gc_OtherModels = gc_Str<"O">;
		constexpr CStr gc_Reset = gc_Str<"\x1b[0m">;
		constexpr CStr gc_Dim = gc_Str<"\x1b[2m">;
		// Between the dimmed labels and the undimmed context percent
		constexpr CStr gc_Highlight = gc_Str<"\x1b[38;5;246m">;
		constexpr CStr gc_ModelInUse = gc_Str<"\x1b[38;5;75m">;

		constexpr CStr gc_BackgroundGreen = gc_Str<"\x1b[42m">;
		constexpr CStr gc_BackgroundYellow = gc_Str<"\x1b[43m">;
		constexpr CStr gc_BackgroundRed = gc_Str<"\x1b[41m">;
		constexpr CStr gc_BackgroundOrange = gc_Str<"\x1b[48;5;208m">;
		constexpr CStr gc_BackgroundEmpty = gc_Str<"\x1b[48;5;236m">;
		constexpr CStr gc_BackgroundAutocompact = gc_Str<"\x1b[48;5;94m">;
		constexpr CStr gc_BackgroundOver = gc_Str<"\x1b[48;5;239m">;

		constexpr CStr gc_ForegroundGreen = gc_Str<"\x1b[32m">;
		constexpr CStr gc_ForegroundYellow = gc_Str<"\x1b[33m">;
		constexpr CStr gc_ForegroundRed = gc_Str<"\x1b[31m">;
		constexpr CStr gc_ForegroundOrange = gc_Str<"\x1b[38;5;208m">;
		constexpr CStr gc_ForegroundEmpty = gc_Str<"\x1b[38;5;236m">;
		// Text inside a filled bar takes the foreground its background can carry: bright white on the
		// dark red, near black on the light orange
		constexpr CStr gc_ForegroundOnDark = gc_Str<"\x1b[97m">;
		constexpr CStr gc_ForegroundOnLight = gc_Str<"\x1b[38;5;232m">;

		int64 fg_Floor(fp64 _Value)
		{
			return _Value.f_Floor().f_ToInt();
		}

		// JavaScript Math.round, halves go up
		int64 fg_Round(fp64 _Value)
		{
			return (_Value + 0.5).f_Floor().f_ToInt();
		}

		// A number and what follows it, on the stack
		using CNumberStr = NStr::CFStr32;

		void fg_AddNumber(CStr::CAppender &_Appender, ch8 const *_pFormat, int64 _Number)
		{
			_Appender += CNumberStr(CNumberStr::CFormat(_pFormat) << _Number);
		}

		// The cells of a bar. A background or foreground is only sent when it differs from the one
		// already in effect, and the bar is reset once at its end
		struct CCellWriter
		{
			CCellWriter(CStr::CAppender &_Appender);

			void f_Text(CStr const &_Background, CStr const &_Foreground, ch8 const *_pCharacter);
			void f_Cell(CStr const &_Background, CStr const *_pForeground, CStr const &_Block);
			void f_End();

			CStr::CAppender &m_Appender;
			CStr const *m_pBackground = nullptr;
			CStr const *m_pForeground = nullptr;
		};

		CCellWriter::CCellWriter(CStr::CAppender &_Appender)
			: m_Appender(_Appender)
		{
		}

		// One column of text on the filled background, for the run-out the bar carries inside it
		void CCellWriter::f_Text(CStr const &_Background, CStr const &_Foreground, ch8 const *_pCharacter)
		{
			if (m_pBackground != &_Background)
			{
				m_Appender += _Background;
				m_pBackground = &_Background;
			}

			if (m_pForeground != &_Foreground)
			{
				m_Appender += _Foreground;
				m_pForeground = &_Foreground;
			}

			m_Appender.f_AddString(_pCharacter, 1);
		}

		void CCellWriter::f_Cell(CStr const &_Background, CStr const *_pForeground, CStr const &_Block)
		{
			if (m_pBackground != &_Background)
			{
				m_Appender += _Background;
				m_pBackground = &_Background;
			}

			if (_pForeground && m_pForeground != _pForeground)
			{
				m_Appender += *_pForeground;
				m_pForeground = _pForeground;
			}

			m_Appender += _Block;
		}

		void CCellWriter::f_End()
		{
			m_Appender += gc_Reset;
		}

		// The context bar: [filled][free][autocompact], 12 cells of 8 sub-steps each. _ZoneStart is
		// the sub-step where the amber autocompact zone begins
		void fg_AddContextBar(CStr::CAppender &_Appender, fp64 _Percent, int64 _ZoneStart, CStr const &_Foreground, CStr const &_Background)
		{
			int64 Total = gc_nContextBarChars * 8;
			int64 Filled = fg_Floor((_Percent * fp64(Total)) / 100.0);
			CCellWriter Cells(_Appender);

			for (aint iCell = 0; iCell < gc_nContextBarChars; ++iCell)
			{
				int64 Start = iCell * 8;
				int64 End = Start + 8;
				// A partly filled cell has one background for what follows the fill, so in a cell the zone
				// starts in, the zone takes it to stay visible
				CStr const &ZoneBackground = End > _ZoneStart ? gc_BackgroundAutocompact : gc_BackgroundEmpty;

				if (Filled >= End)
					Cells.f_Cell(_Background, nullptr, gc_Space);
				else if (Filled > Start)
					Cells.f_Cell(ZoneBackground, &_Foreground, gc_Blocks[8 - (Filled - Start)]);
				else if (End <= _ZoneStart)
					Cells.f_Cell(gc_BackgroundEmpty, nullptr, gc_Space);
				else if (Start >= _ZoneStart)
					Cells.f_Cell(gc_BackgroundAutocompact, nullptr, gc_Space);
				else
				{
					// Cell straddling the free/autocompact boundary
					int64 Sub = _ZoneStart - Start;

					Cells.f_Cell(gc_BackgroundAutocompact, &gc_ForegroundEmpty, gc_Blocks[8 - Sub]);
				}
			}

			Cells.f_End();
		}

		// The pace of a quota: what it consumed relative to the time elapsed in its window. Ratio 1.0 (on
		// track to land exactly on 100% at reset) is a half full bar, which saturates at 2.0. Green
		// <=100%, orange <=150%, red above. A pace that runs the quota out before the reset has the time
		// until that as its text, and a saturated one the real ratio ahead of it. A quota that is used up
		// is a full red bar with the credits left when there is a balance, and that says it is out when
		// there is none. None outside the window
		struct CBurn
		{
			fp64 m_Ratio = 0.0;
			CStr const *m_pForeground = nullptr;
			CStr const *m_pBackground = nullptr;
			CStr const *m_pOnFill = nullptr;
			NStr::CFStr64 m_Text;
			NStr::CFStr64 m_Remaining; // The time left until the window resets
		};

		NStorage::TCOptional<CBurn> fg_Burn(fp64 _Used, int64 _ResetsAt, int64 _WindowSeconds, int64 _Now, NStorage::TCOptional<fp64> const &_Credits)
		{
			int64 Elapsed = _Now - (_ResetsAt - _WindowSeconds);

			if (Elapsed <= 0 || Elapsed > _WindowSeconds)
				return {};

			fp64 Expected = (fp64(Elapsed) / fp64(_WindowSeconds)) * 100.0;

			if (Expected <= 0.0)
				return {};

			bool bUsedUp = _Used >= 100.0;
			fp64 RawRatio = _Used / Expected;
			CBurn Burn;

			Burn.m_Ratio = bUsedUp ? fp64(2.0) : fg_Min(RawRatio, fp64(2.0));

			int64 RatioPercent = fg_Round(Burn.m_Ratio * 100.0);

			if (RatioPercent > 150)
			{
				Burn.m_pForeground = &gc_ForegroundRed;
				Burn.m_pBackground = &gc_BackgroundRed;
				Burn.m_pOnFill = &gc_ForegroundOnDark;
			}
			else if (RatioPercent > 100)
			{
				Burn.m_pForeground = &gc_ForegroundOrange;
				Burn.m_pBackground = &gc_BackgroundOrange;
				Burn.m_pOnFill = &gc_ForegroundOnLight;
			}
			else
			{
				Burn.m_pForeground = &gc_ForegroundGreen;
				Burn.m_pBackground = &gc_BackgroundGreen;
				Burn.m_pOnFill = &gc_ForegroundOnDark;
			}

			NTime::fg_FormatSecondsDurationToHumanReadable(Burn.m_Remaining, fp64(_ResetsAt - _Now));

			// A ratio over 1.0 is exactly when the quota reaches 100% before the reset
			if (bUsedUp && _Credits)
				Burn.m_Text = NStr::CFStr64::CFormat("{} cred") << fg_Floor(*_Credits);
			else if (bUsedUp)
				Burn.m_Text = "out";
			else if (_Used > 0.0 && RawRatio > 1.0)
			{
				// A full bar cannot show how far past the pace this is, so it says so in numbers
				if (RawRatio >= 2.0)
					Burn.m_Text = NStr::CFStr64::CFormat("{}% ") << fg_Round(RawRatio * 100.0);

				NTime::fg_FormatSecondsDurationToHumanReadable(Burn.m_Text, fg_Max((fp64(100.0) - _Used) * (fp64(Elapsed) / _Used), fp64(0.0)));
			}

			return Burn;
		}

		// A burn bar, with the time left until the reset after it. The text is carried inside the filled
		// part of the bar, which grows until it holds the text with a column to spare on either side
		void fg_AddBurnBar(CStr::CAppender &_Appender, CBurn const &_Burn)
		{
			auto fFilledCells = [Ratio = _Burn.m_Ratio](int64 _nWidth)
				{
					return fg_Round((Ratio * fp64(_nWidth * 8)) / 2.0) / 8;
				}
			;

			NStr::CFStr64 const &Text = _Burn.m_Text;
			int64 nWidth = gc_nBurnBarChars;

			while (!Text.f_IsEmpty() && nWidth < 64 && fFilledCells(nWidth) < int64(Text.f_GetLen()) + 2)
				++nWidth;

			int64 Total = nWidth * 8;
			int64 Steps = fg_Round((_Burn.m_Ratio * fp64(Total)) / 2.0);
			int64 Half = Total / 2;
			int64 TextStart = Text.f_IsEmpty() ? 0 : fg_Max(int64(1), (fFilledCells(nWidth) - int64(Text.f_GetLen())) / 2);

			_Appender += gc_Space;

			{
				CCellWriter Cells(_Appender);

				for (int64 iCell = 0; iCell < nWidth; ++iCell)
				{
					if (!Text.f_IsEmpty() && iCell >= TextStart && iCell < TextStart + int64(Text.f_GetLen()))
					{
						Cells.f_Text(*_Burn.m_pBackground, *_Burn.m_pOnFill, Text.f_GetStr() + (iCell - TextStart));
						continue;
					}

					int64 Start = iCell * 8;
					int64 End = Start + 8;
					CStr const &ZoneBackground = Start >= Half ? gc_BackgroundOver : gc_BackgroundEmpty;

					if (Steps >= End)
						Cells.f_Cell(*_Burn.m_pBackground, nullptr, gc_Space);
					else if (Steps > Start)
						Cells.f_Cell(ZoneBackground, _Burn.m_pForeground, gc_Blocks[8 - (Steps - Start)]);
					else
						Cells.f_Cell(ZoneBackground, nullptr, gc_Space);
				}

				Cells.f_End();
			}

			_Appender += gc_Space;
			_Appender += gc_Dim;
			_Appender += _Burn.m_Remaining;
			_Appender += gc_Reset;
		}

		// Two burn bars in one, the top half of each cell for one and the bottom half for the other, in
		// steps of half a cell. A half without a burn is empty. The text of _Shown cannot fit in half a
		// row, so it follows the bar in its colour, and the time left until its reset after that
		void fg_AddSplitBurnBar(CStr::CAppender &_Appender, NStorage::TCOptional<CBurn> const &_Top, NStorage::TCOptional<CBurn> const &_Bottom, CBurn const &_Shown)
		{
			int64 TopSteps = _Top ? fg_Round(_Top->m_Ratio * fp64(gc_nBurnBarChars)) : 0;
			int64 BottomSteps = _Bottom ? fg_Round(_Bottom->m_Ratio * fp64(gc_nBurnBarChars)) : 0;

			_Appender += gc_Space;

			{
				CCellWriter Cells(_Appender);

				for (int64 iCell = 0; iCell < gc_nBurnBarChars; ++iCell)
				{
					int64 Top = fg_Clamp(TopSteps - iCell * 2, int64(0), int64(2));
					int64 Bottom = fg_Clamp(BottomSteps - iCell * 2, int64(0), int64(2));
					CStr const &ZoneBackground = iCell >= gc_nBurnBarChars / 2 ? gc_BackgroundOver : gc_BackgroundEmpty;

					if (Top && Bottom && _Top->m_pForeground != _Bottom->m_pForeground)
					{
						// A cell has two colours, so where both halves are filled in different colours
						// neither can also leave a quarter empty, and a half filled half is shown full
						Cells.f_Cell(*_Bottom->m_pBackground, _Top->m_pForeground, gc_Quadrants[2][2]);
					}
					else if (Top == 2 && Bottom == 2)
						Cells.f_Cell(*_Top->m_pBackground, nullptr, gc_Space);
					else
						Cells.f_Cell(ZoneBackground, Top ? _Top->m_pForeground : Bottom ? _Bottom->m_pForeground : nullptr, gc_Quadrants[Top][Bottom]);
				}

				Cells.f_End();
			}

			_Appender += gc_Space;

			if (!_Shown.m_Text.f_IsEmpty())
			{
				_Appender += *_Shown.m_pForeground;
				_Appender += _Shown.m_Text;
				_Appender += gc_Reset;
				_Appender += gc_Space;
			}

			_Appender += gc_Dim;
			_Appender += _Shown.m_Remaining;
			_Appender += gc_Reset;
		}

		// The calendar month in UTC that _Now is in, as [start, end) in Unix seconds. The usage API does not say
		// when the spend cap resets, and a monthly cap is taken to reset with the month
		void fg_MonthWindow(int64 _Now, int64 &o_Start, int64 &o_End)
		{
			// The convert keeps a pointer to the time it is given
			NTime::CTime Now = NTime::CTimeConvert::fs_FromUnixSeconds(uint64(_Now));
			NTime::CTimeConvert NowConvert(Now);
			int64 Year = NowConvert.f_GetYear();
			aint Month = NowConvert.f_GetMonth();

			o_Start = int64(NTime::CTimeConvert(NTime::CTimeConvert::fs_CreateTime(Year, Month)).f_UnixSeconds());

			if (Month == 12)
				o_End = int64(NTime::CTimeConvert(NTime::CTimeConvert::fs_CreateTime(Year + 1, 1)).f_UnixSeconds());
			else
				o_End = int64(NTime::CTimeConvert(NTime::CTimeConvert::fs_CreateTime(Year, Month + 1)).f_UnixSeconds());
		}

		// A minor amount in whole units of its currency, with the dollar sign in front for USD
		void fg_AddAmount(CStr::CAppender &_Appender, int64 _Minor, int64 _Exponent, bool _bDollars)
		{
			int64 Divisor = 1;

			for (int64 iDigit = 0; iDigit < _Exponent; ++iDigit)
				Divisor *= 10;

			if (_bDollars)
				_Appender += gc_Str<"$">;

			fg_AddNumber(_Appender, "{}", _Minor / Divisor);
		}

		// The spend so far this month against the cap, with a burn bar paced over the month when there is a cap
		void fg_AddSpend(CStr::CAppender &_Appender, CSpendLimit const &_Spend, int64 _Now, bool _bStale)
		{
			bool bDollars = _Spend.m_Currency.f_IsEmpty() || _Spend.m_Currency == "USD";

			_Appender += gc_Space;
			_Appender += gc_Space;
			_Appender += gc_Dim;

			// Without a cap there is no percent, so the amount spent is shown instead
			if (!_Spend.m_LimitMinor || *_Spend.m_LimitMinor <= 0)
			{
				fg_AddAmount(_Appender, _Spend.m_UsedMinor, _Spend.m_Exponent, bDollars);

				if (!bDollars)
				{
					_Appender += gc_Space;
					_Appender += _Spend.m_Currency;
				}

				if (_bStale)
					_Appender += gc_Str<"~">;

				_Appender += gc_Reset;
				return;
			}

			fp64 Percent = (fp64(_Spend.m_UsedMinor) * 100.0) / fp64(*_Spend.m_LimitMinor);

			_Appender += bDollars ? gc_Str<"$"> : _Spend.m_Currency;
			fg_AddNumber(_Appender, ":{}%", fg_Floor(Percent));

			if (_bStale)
				_Appender += gc_Str<"~">;

			_Appender += gc_Reset;

			int64 MonthStart;
			int64 MonthEnd;

			fg_MonthWindow(_Now, MonthStart, MonthEnd);

			if (NStorage::TCOptional<CBurn> Burn = fg_Burn(Percent, MonthEnd, MonthEnd - MonthStart, _Now, {}))
				fg_AddBurnBar(_Appender, *Burn);
		}

		// The first character of the name, or "M" without one
		void fg_AddAbbreviation(CStr::CAppender &_Appender, CStr const &_Name)
		{
			if (_Name.f_IsEmpty())
			{
				_Appender += gc_DefaultModel;
				return;
			}

			ch8 const *pName = _Name.f_GetStr();
			umint Len = 1;

			// The rest of a UTF-8 sequence
			while ((uch8(pName[Len]) & 0xc0) == 0x80)
				++Len;

			_Appender.f_AddString(pName, Len);
		}

		// A weekly limit as the line shows it
		struct CShownLimit
		{
			CStr const *m_pLabel = nullptr;
			fp64 m_Percent = 0.0;
			int64 m_ResetsAt = 0;
			bool m_bStale = false;
		};

		// When the quota runs out at the pace so far in its window, none for a limit that is unused or outside its window
		NStorage::TCOptional<fp64> fg_RunsOutAt(CShownLimit const &_Limit, int64 _Now)
		{
			int64 Elapsed = _Now - (_Limit.m_ResetsAt - gc_Week);

			if (_Limit.m_Percent <= 0.0 || Elapsed <= 0 || Elapsed > gc_Week)
				return {};

			return fp64(_Now) + fg_Max((fp64(100.0) - _Limit.m_Percent) * (fp64(Elapsed) / _Limit.m_Percent), fp64(0.0));
		}

		bool fg_RunsOutBefore(CShownLimit const &_Left, CShownLimit const &_Right, int64 _Now)
		{
			NStorage::TCOptional<fp64> LeftRunsOut = fg_RunsOutAt(_Left, _Now);
			NStorage::TCOptional<fp64> RightRunsOut = fg_RunsOutAt(_Right, _Now);

			if (LeftRunsOut && RightRunsOut)
				return *LeftRunsOut < *RightRunsOut;

			if (LeftRunsOut || RightRunsOut)
				return !!LeftRunsOut;

			return _Left.m_Percent > _Right.m_Percent;
		}
	}

	CStatusLineInput CStatusLineInput::fs_FromJson(NEncoding::CJsonSorted const &_Json)
	{
		CStatusLineInput Return;

		NEncoding::CJsonSorted const *pModel = fg_JsonMember(&_Json, "model");

		Return.m_ModelName = fg_JsonText(fg_JsonMember(pModel, "display_name"));

		NEncoding::CJsonSorted const *pContextWindow = fg_JsonMember(&_Json, "context_window");

		Return.m_UsedPercentage = fg_JsonNumber(fg_JsonMember(pContextWindow, "used_percentage"));
		Return.m_ContextWindowSize = fg_JsonNumber(fg_JsonMember(pContextWindow, "context_window_size"));

		NEncoding::CJsonSorted const *pSevenDay = fg_JsonMember(fg_JsonMember(&_Json, "rate_limits"), "seven_day");

		if (pSevenDay && pSevenDay->f_IsObject())
		{
			if (NEncoding::CJsonSorted const *pWeekly = fg_JsonMember(pSevenDay, "used_percentage"))
			{
				Return.m_bWeeklyPresent = true;
				Return.m_WeeklyText = fg_JsonText(pWeekly);
				Return.m_WeeklyPercent = fg_JsonNumber(pWeekly);
			}

			Return.m_WeeklyResetsAt = fg_ToEpoch(fg_JsonMember(pSevenDay, "resets_at"));
		}

		return Return;
	}

	namespace
	{
		// What JavaScript's String.prototype.trim removes: white space and line terminators
		bool fg_IsJavaScriptWhiteSpace(ch32 _Character)
		{
			switch (_Character)
			{
			case 0x0009:
			case 0x000A:
			case 0x000B:
			case 0x000C:
			case 0x000D:
			case 0x0020:
			case 0x00A0:
			case 0x1680:
			case 0x2028:
			case 0x2029:
			case 0x202F:
			case 0x205F:
			case 0x3000:
			case 0xFEFF:
				return true;
			}

			return _Character >= 0x2000 && _Character <= 0x200A;
		}

		// The code points of _String, with the byte offset each one starts at and one past the last
		struct CCodePoints
		{
			NContainer::TCVector<ch32> m_Characters;
			NContainer::TCVector<umint> m_Offsets;
		};

		CCodePoints fg_CodePoints(CStr const &_String)
		{
			CCodePoints CodePoints;
			umint Offset = 0;
			umint Length = _String.f_GetLen();

			while (Offset < Length)
			{
				NStr::CStrIteratorUTF8 iCharacter(_String.f_GetStr() + Offset, Length - Offset);
				umint CharacterLength = fg_Max(iCharacter.f_GetLastWholeCodePointPos(), umint(1));

				CodePoints.m_Characters.f_Insert(*iCharacter);
				CodePoints.m_Offsets.f_Insert(Offset);
				Offset += CharacterLength;
			}

			CodePoints.m_Offsets.f_Insert(Length);

			return CodePoints;
		}
	}

	// An integer variable of Claude Code's environment, read the way Claude Code reads it: digits grouped by thousands with
	// one separator throughout, `_`, `,`, a space, a no-break space or a narrow no-break space, a number in exponent notation
	// that is an integer, or else the digits the value starts with. None when there are no such digits
	NStorage::TCOptional<int64> fg_ParseClaudeCodeInteger(NStr::CStr const &_Value)
	{
		constexpr int64 c_Limit = int64(1) << 53; // Larger values only need to be larger than any output limit

		CCodePoints CodePoints = fg_CodePoints(_Value);
		umint iFirst = 0;
		umint iLast = CodePoints.m_Characters.f_GetLen();

		while (iFirst < iLast && fg_IsJavaScriptWhiteSpace(CodePoints.m_Characters[iFirst]))
			++iFirst;

		while (iLast > iFirst && fg_IsJavaScriptWhiteSpace(CodePoints.m_Characters[iLast - 1]))
			--iLast;

		umint StartOffset = CodePoints.m_Offsets[iFirst];
		CStr Value = _Value.f_Extract(StartOffset, CodePoints.m_Offsets[iLast] - StartOffset);
		// Claude Code limits the length in UTF-16 code units. A value its patterns match only has characters of the basic
		// multilingual plane, which are one unit each, and longer values fall back to the leading digits either way
		umint nCodeUnits = iLast - iFirst;
		ch8 const *pStart = Value.f_GetStr();
		ch8 const *pEnd = pStart + Value.f_GetLen();
		ch8 const *pParse = pStart;
		bool bNegative = false;

		if (pParse < pEnd && (*pParse == '+' || *pParse == '-'))
			bNegative = *pParse++ == '-';

		auto fIsDigit = [&](ch8 const *_pChar)
			{
				return _pChar < pEnd && *_pChar >= '0' && *_pChar <= '9';
			}
		;

		auto fSeparatorLength = [&](ch8 const *_pChar) -> umint
			{
				umint nLeft = umint(pEnd - _pChar);

				if (nLeft >= 1 && (*_pChar == '_' || *_pChar == ',' || *_pChar == ' '))
					return 1;
				if (nLeft >= 2 && NStr::fg_StrCmp(_pChar, "\xC2\xA0", 2) == 0)
					return 2;
				if (nLeft >= 3 && NStr::fg_StrCmp(_pChar, "\xE2\x80\xAF", 3) == 0)
					return 3;

				return 0;
			}
		;

		auto fSigned = [&](int64 _Magnitude)
			{
				return bNegative ? -_Magnitude : _Magnitude;
			}
		;

		if (nCodeUnits <= 32)
		{
			// [+-]?(\d+(\.\d*)?|\.\d+)[eE][+-]?\d+, which Claude Code converts to a correctly rounded double before it
			// checks for an integer. strtod rounds correctly, where the framework's float parser can land just below
			{
				ch8 const *pNumber = pParse;
				bool bAnyDigit = false;

				while (fIsDigit(pNumber))
				{
					++pNumber;
					bAnyDigit = true;
				}

				if (pNumber < pEnd && *pNumber == '.')
				{
					++pNumber;

					while (fIsDigit(pNumber))
					{
						++pNumber;
						bAnyDigit = true;
					}
				}

				if (bAnyDigit && pNumber < pEnd && (*pNumber == 'e' || *pNumber == 'E'))
				{
					++pNumber;

					if (pNumber < pEnd && (*pNumber == '+' || *pNumber == '-'))
						++pNumber;

					bool bExponentDigit = false;

					while (fIsDigit(pNumber))
					{
						++pNumber;
						bExponentDigit = true;
					}

					if (bExponentDigit && pNumber == pEnd)
					{
						double Number = std::strtod(Value.f_GetStr(), nullptr);

						if (!std::isfinite(Number) || std::floor(Number) != Number)
							return {};

						if (std::fabs(Number) >= double(c_Limit))
							return Number < 0.0 ? -c_Limit : c_Limit;

						return int64(Number);
					}
				}
			}

			ch8 const *pGroup = pParse;
			int64 Grouped = 0;
			umint nLeadingDigits = 0;

			while (fIsDigit(pGroup) && nLeadingDigits < 3)
			{
				Grouped = Grouped * 10 + (*pGroup++ - '0');
				++nLeadingDigits;
			}

			umint SeparatorLength = nLeadingDigits ? fSeparatorLength(pGroup) : 0;

			if (SeparatorLength)
			{
				ch8 const *pSeparator = pGroup;
				umint nGroups = 0;
				bool bGrouped = true;

				while (pGroup < pEnd)
				{
					if (fSeparatorLength(pGroup) != SeparatorLength || NStr::fg_StrCmp(pGroup, pSeparator, SeparatorLength) != 0)
					{
						bGrouped = false;
						break;
					}

					pGroup += SeparatorLength;

					for (umint iDigit = 0; iDigit < 3; ++iDigit)
					{
						if (!fIsDigit(pGroup))
						{
							bGrouped = false;
							break;
						}

						Grouped = fg_Min(Grouped * 10 + (*pGroup++ - '0'), c_Limit);
					}

					if (!bGrouped)
						break;

					++nGroups;
				}

				if (bGrouped && nGroups)
					return fSigned(Grouped);
			}
		}

		int64 Leading = 0;

		if (!fIsDigit(pParse))
			return {};

		while (fIsDigit(pParse))
			Leading = fg_Min(Leading * 10 + (*pParse++ - '0'), c_Limit);

		return fSigned(Leading);
	}

	// The share of the context window after the point where autocompact starts, in tenths of a percent, worked out as
	// Claude Code does. Its effective window is the window less the output reserve, the maximum output up to 20k, and
	// compaction starts the summary buffer below it. The model's own maximum output is not in the status document, so
	// without CLAUDE_CODE_MAX_OUTPUT_TOKENS the reserve is 20k. CLAUDE_AUTOCOMPACT_PCT_OVERRIDE is the percent of the
	// effective window at which compaction starts instead when that is earlier, and counts only above 0 and up to 100.
	// An unknown window size counts as the default window
	int64 fg_AutocompactReserveTenths(fp64 _ContextWindowSize, CAutocompactSettings const &_Settings)
	{
		fp64 ContextWindowSize = _ContextWindowSize > 0.0 ? _ContextWindowSize : fp64(gc_DefaultContextWindowTokens);
		int64 OutputReserve = _Settings.m_MaxOutputTokens > 0 ? fg_Min(_Settings.m_MaxOutputTokens, gc_OutputReserveTokens) : gc_OutputReserveTokens;
		fp64 EffectiveWindow = ContextWindowSize - fp64(OutputReserve);
		fp64 Threshold = EffectiveWindow - fp64(gc_SummaryBufferTokens);
		fp64 PctOverride = _Settings.m_PctOverride;

		if (PctOverride > 0.0 && PctOverride <= 100.0)
			Threshold = fg_Min(fp64(fg_Floor(EffectiveWindow * (PctOverride / 100.0))), Threshold);

		return fg_Floor((ContextWindowSize - Threshold) * 1000.0 / ContextWindowSize);
	}

	// Whether the model in use, as Claude Code displays it, draws from the limit scoped to a model. An unknown model is
	// taken to be the scoped one, and a scoped limit without a model name is taken to cover none
	bool fg_ScopedLimitApplies(NStr::CStr const &_ModelName, CUsageLimit const &_Scoped)
	{
		if (_Scoped.m_Name.f_IsEmpty())
			return false;

		return _ModelName.f_IsEmpty() || _ModelName.f_CmpNoCase(_Scoped.m_Name, _Scoped.m_Name.f_GetLen()) == 0;
	}

	CRenderResult fg_RenderStatusLine(CStatusLineInput const &_Input, CStatusCache const &_Cache, int64 _Now, CAutocompactSettings const &_Autocompact)
	{
		CRenderResult Return;

		int64 Tenths = fg_AutocompactReserveTenths(_Input.m_ContextWindowSize, _Autocompact);
		int64 Percent = fg_Floor(_Input.m_UsedPercentage);
		CStr const *pForeground;
		CStr const *pBackground;

		if (Percent >= 90)
		{
			pForeground = &gc_ForegroundRed;
			pBackground = &gc_BackgroundRed;
		}
		else if (Percent >= 70)
		{
			pForeground = &gc_ForegroundYellow;
			pBackground = &gc_BackgroundYellow;
		}
		else
		{
			pForeground = &gc_ForegroundGreen;
			pBackground = &gc_BackgroundGreen;
		}

		int64 Total = gc_nContextBarChars * 8;
		int64 ZoneStart = ((1000 - Tenths) * Total) / 1000;
		int64 UntilCompact = fg_Max((1000 - Tenths) / 10 - Percent, int64(0));

		{
			CStr::CAppender Appender(Return.m_Line);

			fg_AddContextBar(Appender, _Input.m_UsedPercentage, ZoneStart, *pForeground, *pBackground);
			fg_AddNumber(Appender, " {}%", UntilCompact);

			// Both weekly limits: the scoped one with its model's initial, and the one covering every
			// model, from the document when it carries one and from the cache until the session has
			// made a request. The percent of the one that runs out first is brighter than the other's. The
			// label of the limit the model in use draws from is coloured, and the times after the bar are its
			CUsageLimit const *pScoped = _Cache.m_Claude && _Cache.m_Claude->m_Scoped ? &*_Cache.m_Claude->m_Scoped : nullptr;
			CUsageLimit const *pAllModels = _Cache.m_Claude && _Cache.m_Claude->m_AllModels ? &*_Cache.m_Claude->m_AllModels : nullptr;
			bool bScopedApplies = pScoped && fg_ScopedLimitApplies(_Input.m_ModelName, *pScoped);
			// The status document's own limit is current, but what the cache holds is as old as the last refresh that
			// succeeded
			bool bClaudeStale = _Cache.m_Claude && _Now - _Cache.m_Claude->m_FetchedAt >= gc_ClaudeStaleAfter;

			CShownLimit Limits[2];
			aint nLimits = 0;
			aint iInUse = 0;

			if (pScoped)
			{
				Limits[nLimits++] = CShownLimit
					{
						.m_pLabel = pScoped->m_Name.f_IsEmpty() ? &gc_DefaultModel : &pScoped->m_Name
						, .m_Percent = pScoped->m_Percent
						, .m_ResetsAt = pScoped->m_ResetsAt
						, .m_bStale = bClaudeStale
					}
				;
			}

			if (_Input.m_bWeeklyPresent || pAllModels)
			{
				if (!bScopedApplies)
					iInUse = nLimits;

				CShownLimit &Limit = Limits[nLimits++];

				Limit.m_pLabel = bScopedApplies ? &gc_OtherModels : _Input.m_ModelName.f_IsEmpty() ? &gc_DefaultModel : &_Input.m_ModelName;

				if (_Input.m_bWeeklyPresent)
				{
					Limit.m_Percent = _Input.m_WeeklyPercent;
					Limit.m_ResetsAt = _Input.m_WeeklyResetsAt ? *_Input.m_WeeklyResetsAt : pAllModels ? pAllModels->m_ResetsAt : 0;
				}
				else
				{
					Limit.m_Percent = pAllModels->m_Percent;
					Limit.m_ResetsAt = pAllModels->m_ResetsAt;
					Limit.m_bStale = bClaudeStale;
				}
			}

			if (nLimits)
			{
				aint iFirst = nLimits == 2 && fg_RunsOutBefore(Limits[1], Limits[0], _Now) ? 1 : 0;

				// Two spaces set the Claude and codex usage sections apart from the context section
				Appender += gc_Space;

				for (aint iLimit = 0; iLimit < nLimits; ++iLimit)
				{
					bool bInUse = nLimits == 2 && iLimit == iInUse;
					bool bFirst = nLimits == 2 && iLimit == iFirst;

					Appender += gc_Space;
					Appender += bInUse ? gc_ModelInUse : gc_Dim;
					fg_AddAbbreviation(Appender, *Limits[iLimit].m_pLabel);

					if (bInUse)
					{
						Appender += gc_Reset;
						Appender += gc_Dim;
					}

					Appender += gc_Str<":">;

					// Dim is an attribute of its own, which a colour set after it would keep
					if (bFirst)
					{
						Appender += gc_Reset;
						Appender += gc_Highlight;
					}

					fg_AddNumber(Appender, "{}%", fg_Floor(Limits[iLimit].m_Percent));

					if (Limits[iLimit].m_bStale)
						Appender += gc_Str<"~">;

					Appender += gc_Reset;
				}

				NStorage::TCOptional<CBurn> Burns[2];

				for (aint iLimit = 0; iLimit < nLimits; ++iLimit)
					Burns[iLimit] = fg_Burn(Limits[iLimit].m_Percent, Limits[iLimit].m_ResetsAt, gc_Week, _Now, {});

				// The scoped limit is the top half and the one covering every model the bottom
				if (nLimits == 2 && (Burns[0] || Burns[1]))
					fg_AddSplitBurnBar(Appender, Burns[0], Burns[1], Burns[iInUse] ? *Burns[iInUse] : *Burns[1 - iInUse]);
				else if (nLimits == 1 && Burns[0])
					fg_AddBurnBar(Appender, *Burns[0]);
			}

			// An account billed by usage has a monthly spend cap rather than weekly limits
			if (!nLimits && _Cache.m_Claude && _Cache.m_Claude->m_Spend)
				fg_AddSpend(Appender, *_Cache.m_Claude->m_Spend, _Now, bClaudeStale);

			// A claimed refresh writes the entry before any fetch has succeeded
			if (_Cache.m_Codex && _Cache.m_Codex->m_FetchedAt > 0)
			{
				CCodexCache const &Codex = *_Cache.m_Codex;

				Appender += gc_Space;
				Appender += gc_Space;
				Appender += gc_Dim;
				fg_AddNumber(Appender, "Cx:{}%", fg_Floor(Codex.m_Percent));

				if (Codex.m_SnapshotAt > 0 && _Now - Codex.m_SnapshotAt >= gc_CodexStaleAfter)
					Appender += gc_Str<"~">;

				Appender += gc_Reset;
				if (NStorage::TCOptional<CBurn> Burn = fg_Burn(Codex.m_Percent, Codex.m_ResetsAt, Codex.m_WindowSeconds, _Now, Codex.m_Credits))
					fg_AddBurnBar(Appender, *Burn);
			}
		}

		// A refresh that was started but has not replaced the entry yet, or failed, counts as the
		// entry's age too, so the usage endpoints see one request per interval whatever happens
		int64 ClaudeAge = _Now - (_Cache.m_Claude ? fg_Max(_Cache.m_Claude->m_FetchedAt, _Cache.m_Claude->m_AttemptedAt) : int64(0));
		bool bWeeklyChanged = _Input.m_bWeeklyPresent && _Input.m_WeeklyText != (_Cache.m_Claude ? _Cache.m_Claude->m_SeenWeekly : CStr{});

		if ((ClaudeAge >= gc_ClaudeMaxAge || bWeeklyChanged) && ClaudeAge >= gc_ClaudeMinRefresh)
			Return.m_StaleTargets.f_Insert("claude");

		if (_Now - (_Cache.m_Codex ? fg_Max(_Cache.m_Codex->m_FetchedAt, _Cache.m_Codex->m_AttemptedAt) : int64(0)) >= gc_CodexMaxAge)
			Return.m_StaleTargets.f_Insert("codex");

		return Return;
	}
}
