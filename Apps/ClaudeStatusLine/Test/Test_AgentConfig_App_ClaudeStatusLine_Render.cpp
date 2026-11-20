// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Test/Test>

#include "../Source/AgentConfig_App_ClaudeStatusLine_Render.h"

using namespace NMib;
using namespace NMib::NAgentConfig::NClaudeStatusLine;

namespace
{
	struct CRender_Tests : public NMib::NTest::CTest
	{
		void f_DoTests();
	};

	void CRender_Tests::f_DoTests()
	{
		DMibTestSuite("AutocompactReserve")
		{
			DMibTestCategory("Default")
			{
				DMibExpect(fg_AutocompactReserveTenths(200000.0, {}), ==, 165);
				DMibExpect(fg_AutocompactReserveTenths(1000000.0, {}), ==, 33);
				DMibExpect(fg_AutocompactReserveTenths(0.0, {}), ==, 165);
			};

			DMibTestCategory("Override")
			{
				// The override is the percent of the effective window, the window less 20k for output, at which compaction starts
				DMibExpect(fg_AutocompactReserveTenths(200000.0, {.m_PctOverride = 80.0}), ==, 280);
				DMibExpect(fg_AutocompactReserveTenths(200000.0, {.m_PctOverride = 50.0}), ==, 550);
				DMibExpect(fg_AutocompactReserveTenths(1000000.0, {.m_PctOverride = 80.0}), ==, 216);
				DMibExpect(fg_AutocompactReserveTenths(1000000.0, {.m_PctOverride = 92.5}), ==, 93);
				DMibExpect(fg_AutocompactReserveTenths(0.0, {.m_PctOverride = 70.0}), ==, 370);
			};

			DMibTestCategory("OverrideAfterDefault")
			{
				DMibExpect(fg_AutocompactReserveTenths(200000.0, {.m_PctOverride = 95.0}), ==, 165);
				DMibExpect(fg_AutocompactReserveTenths(1000000.0, {.m_PctOverride = 99.0}), ==, 33);
				DMibExpect(fg_AutocompactReserveTenths(1000000.0, {.m_PctOverride = 150.0}), ==, 33);
			};

			DMibTestCategory("MaxOutputTokens")
			{
				// A maximum output below 20k makes the output reserve, and so the effective window, that size
				DMibExpect(fg_AutocompactReserveTenths(200000.0, {.m_MaxOutputTokens = 4096}), ==, 85);
				DMibExpect((fg_AutocompactReserveTenths(200000.0, CAutocompactSettings{.m_PctOverride = 80.0, .m_MaxOutputTokens = 4096})), ==, 216);
				DMibExpect(fg_AutocompactReserveTenths(200000.0, {.m_MaxOutputTokens = 64000}), ==, 165);
			};
		};

		DMibTestSuite("ParseClaudeCodeInteger")
		{
			auto fCheck = [](NStr::CStr const &_Case, NStr::CStr const &_Value, NStorage::TCOptional<int64> const &_Expected)
				{
					DMibTestPath(_Case);
					NStorage::TCOptional<int64> Parsed = fg_ParseClaudeCodeInteger(_Value);

					DMibExpect(bool(Parsed), ==, bool(_Expected));
					if (Parsed && _Expected)
						DMibExpect(*Parsed, ==, *_Expected);
				}
			;

			fCheck("Plain", "4096", 4096);
			fCheck("Trimmed", " 4096 ", 4096);
			fCheck("Comma", "4,096", 4096);
			fCheck("Underscore", "1_000_000", 1000000);
			fCheck("NoBreakSpace", "4\xC2\xA0" "096", 4096);
			fCheck("NarrowNoBreakSpace", "4\xE2\x80\xAF" "096", 4096);
			fCheck("Exponent", "4.096e3", 4096);
			fCheck("ExponentFraction", "4.0961e3", {});
			fCheck("ExponentDecimal", "8.2e2", 820);
			fCheck("ExponentLeadingPoint", ".5e1", 5);
			fCheck("ExponentNegative", "4096000e-3", 4096);
			fCheck("ExponentHuge", "1e300", int64(1) << 53);
			fCheck("ExponentRoundsToInteger", "4.095999999999999999e3", 4096);
			fCheck("MixedSeparators", "1,000_000", 1);
			fCheck("BadGroup", "4,09", 4);
			fCheck("LeadingDigits", "4096tokens", 4096);
			fCheck("Negative", "-4,096", -4096);
			fCheck("TrailingNoBreakSpace", "4,096\xC2\xA0", 4096);
			fCheck("LeadingIdeographicSpace", "\xE3\x80\x80" "4096", 4096);
			// 37 bytes but 25 characters, so within Claude Code's limit of 32, and above the 2^53 the parser stops at
			fCheck("LongUnicodeGrouping", "1\xE2\x80\xAF" "000\xE2\x80\xAF" "000\xE2\x80\xAF" "000\xE2\x80\xAF" "000\xE2\x80\xAF" "000\xE2\x80\xAF" "000", int64(1) << 53);
			fCheck("NoDigits", "many", {});
			fCheck("Empty", "", {});
		};

		DMibTestSuite("ScopedLimitApplies")
		{
			CUsageLimit Fable{.m_Name = "Fable"};

			DMibExpectTrue(fg_ScopedLimitApplies("Fable 5.2", Fable));
			DMibExpectTrue(fg_ScopedLimitApplies("fable 5.2", Fable));
			DMibExpectTrue(fg_ScopedLimitApplies("", Fable));
			DMibExpectFalse(fg_ScopedLimitApplies("Opus 5.5", Fable));

			DMibTestCategory("WithoutName")
			{
				CUsageLimit Unnamed;

				DMibExpectFalse(fg_ScopedLimitApplies("Opus 5.5", Unnamed));
				DMibExpectFalse(fg_ScopedLimitApplies("", Unnamed));
			};
		};
	}
}

DMibTestRegister(CRender_Tests, AgentConfig::ClaudeStatusLine);
