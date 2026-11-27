/**
 * =============================================================================
 * Accelerator Extension
 * Copyright (C) 2009-2010 Asher Baker (asherkin).  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * @file crash_parse.h
 * @brief Turns a minidump into the few strings the upload thread needs.
 *
 * breakpad's processor trusts the file it is given and runs inside the server,
 * so a dump it cannot survive takes the server with it -- and because the dump
 * is still there when the server comes back, it takes the server with it again,
 * and again. That is not hypothetical: a dump written by the engine's own crash
 * handler faulted the upload thread while it was building the per-thread part of
 * the crash signature, about 1.2 seconds into every boot.
 *
 * Two things here prevent a repeat. Validate() rejects a file that cannot be a
 * minidump before breakpad sees it at all, and Parse() runs the processor in a
 * child process where the platform can fork one, so a fault while parsing costs
 * one child process instead of the whole server.
 */

#ifndef _INCLUDE_ACCELERATOR_CRASH_PARSE_H_
#define _INCLUDE_ACCELERATOR_CRASH_PARSE_H_

#include <stddef.h>
#include <string>
#include <vector>

namespace crashparse {

/**
 * @brief One loaded module, as the minidump describes it.
 *
 * Only what the upload thread actually uses is kept: the paths to send with a
 * binary upload, the identifiers the server matches on, and the debug file to
 * read symbols out of.
 */
struct ModuleInfo
{
	std::string codeFile;
	std::string debugFile;
	std::string debugIdentifier;
	std::string codeIdentifier;
};

/**
 * @brief The result of looking at a minidump.
 */
struct ParseResult
{
	/** @brief True when @a summary and @a modules are usable. */
	bool parsed = false;
	/** @brief Why the dump could not be used, when @a parsed is false. */
	std::string failure;
	/** @brief The CrashSignature line that is sent to the crash server. */
	std::string summary;
	/** @brief Modules in the order the server indexes them. */
	std::vector<ModuleInfo> modules;
	/** @brief Path of the main executable, used to classify the other modules. */
	std::string mainModuleCodeFile;
};

/**
 * @brief Checks that a file is structurally a minidump.
 *
 * Cheap on purpose: the checks are the ones whose failure breakpad would
 * otherwise discover by walking off the end of the file, on a thread that
 * cannot survive it. A dump that fails here is not handed to the parser.
 *
 * @param path			File to check.
 * @param reason		Receives a short explanation when the check fails.
 * @param reasonLength	Size of @a reason.
 * @return				True when the file may be given to breakpad.
 */
bool Validate(const char *path, char *reason, size_t reasonLength);

/**
 * @brief Whether Parse() survives a fault in breakpad's processor.
 *
 * True where a child process can be forked for it. Where it is false, a dump
 * the parser cannot handle takes the whole process down, so a caller that
 * cannot first move such a dump out of the way should not parse it at all.
 */
bool ParseRunsInOwnProcess();

/**
 * @brief Parses a minidump and builds the crash signature for it.
 *
 * On Linux the parse runs in a forked child: a fault in breakpad's processor
 * kills only that child, and the result says so instead of the server dying.
 * Where no child can be forked the parse runs in the caller.
 *
 * @param path		Dump to parse.
 * @param result	Receives the signature and the module table.
 * @return			True when @a result->parsed is true.
 */
bool Parse(const char *path, ParseResult *result);

}

#endif // _INCLUDE_ACCELERATOR_CRASH_PARSE_H_
