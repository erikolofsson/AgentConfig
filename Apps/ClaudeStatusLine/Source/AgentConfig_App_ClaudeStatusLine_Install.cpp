// Copyright © Erik Olofsson
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/File/File>
#include <Mib/String/UnicodeConversion>

#include "AgentConfig_App_ClaudeStatusLine_Install.h"

namespace NMib::NAgentConfig::NClaudeStatusLine
{
	using namespace NMib::NStr;

	namespace
	{
		// White space and quotation marks of Unicode beyond ASCII, which PowerShell splits on or takes as quotes
		bool fg_IsPowerShellSpecial(ch32 _Character)
		{
			switch (_Character)
			{
			case 0x0085:
			case 0x00A0:
			case 0x1680:
			case 0x2028:
			case 0x2029:
			case 0x202F:
			case 0x205F:
			case 0x3000:
				return true;
			}

			return (_Character >= 0x2000 && _Character <= 0x200A) || (_Character >= 0x2018 && _Character <= 0x201E);
		}

		// A path of the characters that neither Bash nor PowerShell splits or expands in a command name
		bool fg_ShellQuoteNeeded(CStr const &_Path)
		{
			for (NStr::CStrIteratorUTF8 iCharacter(_Path); iCharacter; ++iCharacter)
			{
				ch32 Character = *iCharacter;

				if (Character >= 0x80)
				{
					if (fg_IsPowerShellSpecial(Character))
						return true;

					continue;
				}

				if ((Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9'))
					continue;

				switch (Character)
				{
				case '/':
				case '.':
				case '_':
				case '-':
				case ':':
				case '+':
				case '@':
					continue;
				}

				return true;
			}

			return false;
		}

		// PowerShell takes the typographic single quotes as quotes too, and any of them doubled as one in the string
		CStr fg_PowerShellSingleQuoted(CStr const &_Path)
		{
			CStr Quoted = _Path.f_Replace("'", "''");

			for (ch8 const *pQuote : {"\xE2\x80\x98", "\xE2\x80\x99", "\xE2\x80\x9A", "\xE2\x80\x9B"})
				Quoted = Quoted.f_Replace(pQuote, CStr("{}{}"_f << pQuote << pQuote).f_GetStr());

			return "'{}'"_f << Quoted;
		}

#ifdef DPlatformFamily_Windows
		bool fg_FileExists(CStr const &_Path)
		{
			return NFile::CFile::fs_FileExists(_Path.f_ReplaceChar('\\', '/'), NFile::EFileAttrib_File);
		}

		// Where Claude Code finds Git Bash: CLAUDE_CODE_GIT_BASH_PATH when it names an existing bash or sh, the default
		// install directories, and the Git of the git found on the PATH
		bool fg_GitBashFound()
		{
			CStr ConfiguredPath = fg_GetSys()->f_GetEnvironmentVariable("CLAUDE_CODE_GIT_BASH_PATH");

			if (ConfiguredPath)
			{
				CStr FileName = NFile::CFile::fs_GetFile(ConfiguredPath.f_ReplaceChar('\\', '/')).f_LowerCase();

				if ((FileName == "bash.exe" || FileName == "sh.exe" || FileName == "bash" || FileName == "sh") && fg_FileExists(ConfiguredPath))
					return true;
			}

			if (fg_FileExists("C:/Program Files/Git/bin/bash.exe") || fg_FileExists("C:/Program Files (x86)/Git/bin/bash.exe"))
				return true;

			NContainer::TCVector<CStr> Extensions;
			CStr PathExtensions = fg_GetSys()->f_GetEnvironmentVariable("PATHEXT").f_Trim();

			if (!PathExtensions)
				PathExtensions = ".COM;.EXE;.BAT;.CMD";

			// The extensions Claude Code finds git with, in the order of PATHEXT. A PATHEXT without any of them finds none
			for (CStr const &Extension : PathExtensions.f_Split(";"))
			{
				CStr Trimmed = Extension.f_Trim().f_UpperCase();

				if ((Trimmed == ".COM" || Trimmed == ".EXE" || Trimmed == ".BAT" || Trimmed == ".CMD") && Extensions.f_Contains(Trimmed) < 0)
					Extensions.f_Insert(fg_Move(Trimmed));
			}

			auto fComparable = [](CStr const &_Directory)
				{
					return fg_NormalizedPath(_Directory).f_LowerCase();
				}
			;

			// Claude Code does not take a git from its current directory, which is also --install's
			CStr CurrentDirectory = fComparable(NFile::CFile::fs_GetCurrentDirectory());

			for (CStr const &Entry : fg_GetSys()->f_GetEnvironmentVariable("PATH").f_Split(";"))
			{
				// An entry can be in double quotes, which Claude Code strips before it looks for git
				CStr Directory = fg_NormalizedPath(Entry.f_Replace("\"", "").f_Trim());

				// Claude Code skips relative entries
				if (!Directory || !NFile::CFile::fs_IsPathAbsolute(Directory) || fComparable(Directory) == CurrentDirectory)
					continue;

				// git with each of the extensions Windows runs, as git.cmd for a portable Git
				for (CStr const &Extension : Extensions)
				{
					CStr GitPath = NFile::CFile::fs_AppendPath(Directory, CStr("git{}"_f << Extension));

					if (fg_FileExists(GitPath))
						return fg_FileExists(NFile::CFile::fs_AppendPath(NFile::CFile::fs_GetPath(NFile::CFile::fs_GetPath(GitPath)), "bin/bash.exe"));
				}
			}

			return false;
		}
#endif
	}

	// _Path with forward slashes and its . and .. segments resolved, as path.join leaves a PATH entry such as C:\Git\cmd\.
	CStr fg_NormalizedPath(CStr const &_Path)
	{
		CStr Path = _Path.f_ReplaceChar('\\', '/');
		CStr Prefix = Path.f_StartsWith("//") ? CStr("//") : Path.f_StartsWith("/") ? CStr("/") : CStr();
		NContainer::TCVector<CStr> Segments;

		for (CStr const &Segment : Path.f_Split("/"))
		{
			if (!Segment || Segment == ".")
				continue;

			if (Segment == "..")
			{
				bool bAtRoot = Segments.f_IsEmpty() ? !!Prefix : Segments.f_GetLast().f_EndsWith(":");

				if (bAtRoot)
					continue;

				if (!Segments.f_IsEmpty() && Segments.f_GetLast() != "..")
				{
					Segments.f_Remove(Segments.f_GetLen() - 1);
					continue;
				}
			}

			Segments.f_Insert(Segment);
		}

		CStr Normalized = Prefix;

		for (CStr const &Segment : Segments)
		{
			if (Normalized && !Normalized.f_EndsWith("/"))
				Normalized += "/";

			Normalized += Segment;
		}

		// A drive is only absolute, and only the root, with its slash
		if (Normalized.f_EndsWith(":"))
			Normalized += "/";

		return Normalized;
	}

	// The shell Claude Code runs the status line command with: Bash, which on Windows is Git Bash when Claude Code finds
	// it, and PowerShell otherwise
	EStatusLineShell fg_StatusLineShell()
	{
#ifdef DPlatformFamily_Windows
		return fg_GitBashFound() ? EStatusLineShell::mc_Bash : EStatusLineShell::mc_PowerShell;
#else
		return EStatusLineShell::mc_Bash;
#endif
	}

	// The command that runs _ProgramPath with --status-line in _Shell. A path that needs quoting is quoted for that shell,
	// and PowerShell also needs its call operator to run a quoted path
	CStr fg_StatusLineCommand(CStr const &_ProgramPath, EStatusLineShell _Shell)
	{
		if (!fg_ShellQuoteNeeded(_ProgramPath))
			return "{} --status-line"_f << _ProgramPath;

		if (_Shell == EStatusLineShell::mc_PowerShell)
			return "& {} --status-line"_f << fg_PowerShellSingleQuoted(_ProgramPath);

		return "{} --status-line"_f << fg_StrEscapeBashSingleQuotes(_ProgramPath);
	}
}
