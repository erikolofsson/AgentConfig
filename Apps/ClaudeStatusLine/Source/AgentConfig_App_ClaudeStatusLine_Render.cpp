// Copyright © Erik Olofsson

#include <Mib/Core/Core>
#include <Mib/String/Appender>
#include <Mib/Time/TimeSpan>

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
		constexpr int64 gc_AutocompactTokens = 33000;
		constexpr int64 gc_Week = 604800;
		constexpr aint gc_nContextBarChars = 12;
		constexpr aint gc_nBurnBarChars = 12;

		// Compile-time strings, so the appends know their lengths
		constexpr CStr gc_Blocks[] = {gc_Str<"█">, gc_Str<"▉">, gc_Str<"▊">, gc_Str<"▋">, gc_Str<"▌">, gc_Str<"▍">, gc_Str<"▎">, gc_Str<"▏">};
		constexpr CStr gc_Space = gc_Str<" ">;
		constexpr CStr gc_DefaultModel = gc_Str<"M">;
		constexpr CStr gc_Reset = gc_Str<"\x1b[0m">;
		constexpr CStr gc_Dim = gc_Str<"\x1b[2m">;

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
			CCellWriter(CStr::CAppender &_Appender)
				: m_Appender(_Appender)
			{
			}

			// One column of text on the filled background, for the run-out the bar carries inside it
			void f_Text(CStr const &_Background, CStr const &_Foreground, ch8 const *_pCharacter)
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

			void f_Cell(CStr const &_Background, CStr const *_pForeground, CStr const &_Block)
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

			void f_End()
			{
				m_Appender += gc_Reset;
			}

			CStr::CAppender &m_Appender;
			CStr const *m_pBackground = nullptr;
			CStr const *m_pForeground = nullptr;
		};

		// The context bar: [filled][free][autocompact], 12 cells of 8 sub-steps each. _ZoneStart is
		// the sub-step where the amber autocompact zone begins
		void fg_AddContextBar(CStr::CAppender &_Appender, fp64 _Percent, int64 _ZoneStart, CStr const &_Foreground, CStr const &_Background)
		{
			int64 Total = gc_nContextBarChars * 8;
			int64 Filled = fg_Floor((_Percent * fp64(Total)) / 100.0);
			CCellWriter Cells(_Appender);

			for (aint i = 0; i < gc_nContextBarChars; ++i)
			{
				int64 Start = i * 8;
				int64 End = Start + 8;
				CStr const &ZoneBackground = Start >= _ZoneStart ? gc_BackgroundAutocompact : gc_BackgroundEmpty;

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

		// Burn-rate bar: quota consumed relative to time elapsed in its window. Ratio 1.0 (on track
		// to land exactly on 100% at reset) renders half full and saturates at 2.0. Green <=100%,
		// orange <=150%, red above, and the time left until the reset after it. A pace that runs the
		// quota out before the reset carries the time until that inside the filled part of the bar,
		// which grows until it holds the text with a column to spare on either side; a full bar has
		// room for the real ratio ahead of it. Nothing is added outside the window
		void fg_AddBurnBar(CStr::CAppender &_Appender, fp64 _Used, int64 _ResetsAt, int64 _WindowSeconds, int64 _Now)
		{
			int64 Elapsed = _Now - (_ResetsAt - _WindowSeconds);

			if (Elapsed <= 0 || Elapsed > _WindowSeconds)
				return;

			fp64 Expected = (fp64(Elapsed) / fp64(_WindowSeconds)) * 100.0;

			if (Expected <= 0.0)
				return;

			fp64 RawRatio = _Used / Expected;
			fp64 Ratio = fg_Min(RawRatio, fp64(2.0));
			int64 RatioPercent = fg_Round(Ratio * 100.0);

			CStr const *pForeground;
			CStr const *pBackground;
			CStr const *pOnFill;

			if (RatioPercent > 150)
			{
				pForeground = &gc_ForegroundRed;
				pBackground = &gc_BackgroundRed;
				pOnFill = &gc_ForegroundOnDark;
			}
			else if (RatioPercent > 100)
			{
				pForeground = &gc_ForegroundOrange;
				pBackground = &gc_BackgroundOrange;
				pOnFill = &gc_ForegroundOnLight;
			}
			else
			{
				pForeground = &gc_ForegroundGreen;
				pBackground = &gc_BackgroundGreen;
				pOnFill = &gc_ForegroundOnDark;
			}

			// The time left until the window resets, and when the quota is predicted to reach 100% at
			// the pace so far. A ratio over 1.0 is exactly when that lands before the reset
			NStr::CFStr64 Remaining;
			NStr::CFStr64 Text;

			NTime::fg_FormatSecondsDurationToHumanReadable(Remaining, fp64(_ResetsAt - _Now));

			if (_Used > 0.0 && RawRatio > 1.0)
			{
				// A full bar cannot show how far past the pace this is, so it says so in numbers
				if (RawRatio >= 2.0)
					Text = NStr::CFStr64::CFormat("{}% ") << fg_Round(RawRatio * 100.0);

				NTime::fg_FormatSecondsDurationToHumanReadable(Text, fg_Max((fp64(100.0) - _Used) * (fp64(Elapsed) / _Used), fp64(0.0)));
			}

			auto fFilledCells = [Ratio](int64 _nWidth)
				{
					return fg_Round((Ratio * fp64(_nWidth * 8)) / 2.0) / 8;
				}
			;

			int64 nWidth = gc_nBurnBarChars;

			// Grown until the filled part holds the text and a column on either side of it
			while (!Text.f_IsEmpty() && nWidth < 64 && fFilledCells(nWidth) < int64(Text.f_GetLen()) + 2)
				++nWidth;

			int64 Total = nWidth * 8;
			int64 Steps = fg_Round((Ratio * fp64(Total)) / 2.0);
			int64 Half = Total / 2;
			int64 TextStart = Text.f_IsEmpty() ? 0 : fg_Max(int64(1), (fFilledCells(nWidth) - int64(Text.f_GetLen())) / 2);

			_Appender += gc_Space;

			{
				CCellWriter Cells(_Appender);

				for (int64 i = 0; i < nWidth; ++i)
				{
					if (!Text.f_IsEmpty() && i >= TextStart && i < TextStart + int64(Text.f_GetLen()))
					{
						Cells.f_Text(*pBackground, *pOnFill, Text.f_GetStr() + (i - TextStart));
						continue;
					}

					int64 Start = i * 8;
					int64 End = Start + 8;
					CStr const &ZoneBackground = Start >= Half ? gc_BackgroundOver : gc_BackgroundEmpty;

					if (Steps >= End)
						Cells.f_Cell(*pBackground, nullptr, gc_Space);
					else if (Steps > Start)
						Cells.f_Cell(ZoneBackground, pForeground, gc_Blocks[8 - (Steps - Start)]);
					else
						Cells.f_Cell(ZoneBackground, nullptr, gc_Space);
				}

				Cells.f_End();
			}

			_Appender += gc_Space;
			_Appender += gc_Dim;
			_Appender += Remaining;
			_Appender += gc_Reset;
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

	CRenderResult fg_RenderStatusLine(CStatusLineInput const &_Input, CStatusCache const &_Cache, int64 _Now, fp64 _AutocompactOverride)
	{
		CRenderResult Return;

		// Autocompact reserve is a fixed ~33k tokens, so as a share of the window it shrinks as
		// the window grows. Expressed in tenths of a percent
		int64 Tenths;

		if (_Input.m_ContextWindowSize > 0.0 && _AutocompactOverride == 0.0)
			Tenths = fg_Floor(fp64(gc_AutocompactTokens * 1000) / _Input.m_ContextWindowSize);
		else
			Tenths = fg_Max(fg_Floor(_AutocompactOverride * 10.0), int64(165));

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

			// One weekly limit for the model in use: its own scoped limit when it has one, otherwise
			// the limit shared by all models, from the document when it carries one and from the
			// cache when it does not, which is the case until the session has made a request.
			// Labelled with the model's initial
			CUsageLimit const *pScoped = _Cache.m_Claude && _Cache.m_Claude->m_Scoped ? &*_Cache.m_Claude->m_Scoped : nullptr;
			CUsageLimit const *pAllModels = _Cache.m_Claude && _Cache.m_Claude->m_AllModels ? &*_Cache.m_Claude->m_AllModels : nullptr;
			bool bScopedApplies = pScoped
				&& (_Input.m_ModelName.f_IsEmpty() || _Input.m_ModelName.f_CmpNoCase(pScoped->m_Name, pScoped->m_Name.f_GetLen()) == 0)
			;

			if (bScopedApplies || _Input.m_bWeeklyPresent || pAllModels)
			{
				fp64 Percent;
				int64 ResetsAt = _Input.m_WeeklyResetsAt ? *_Input.m_WeeklyResetsAt : 0;

				if (bScopedApplies)
				{
					Percent = pScoped->m_Percent;

					if (pScoped->m_ResetsAt)
						ResetsAt = pScoped->m_ResetsAt;
				}
				else if (_Input.m_bWeeklyPresent)
					Percent = _Input.m_WeeklyPercent;
				else
				{
					Percent = pAllModels->m_Percent;
					ResetsAt = pAllModels->m_ResetsAt;
				}

				// Two spaces set the Claude and codex usage sections apart from the context section
				Appender += gc_Space;
				Appender += gc_Space;
				Appender += gc_Dim;
				CStr const &Label = !_Input.m_ModelName.f_IsEmpty() ? _Input.m_ModelName : pScoped && !pScoped->m_Name.f_IsEmpty() ? pScoped->m_Name : gc_DefaultModel;

				fg_AddAbbreviation(Appender, Label);
				fg_AddNumber(Appender, ":{}%", fg_Floor(Percent));
				Appender += gc_Reset;
				fg_AddBurnBar(Appender, Percent, ResetsAt, gc_Week, _Now);
			}

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
				fg_AddBurnBar(Appender, Codex.m_Percent, Codex.m_ResetsAt, Codex.m_WindowSeconds, _Now);
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
