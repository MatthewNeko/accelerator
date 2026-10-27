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
 * @file sp_stack.h
 * @brief Captures the live SourcePawn call stack so crash reports say which
 *        plugin, function and source line was running, not just which C++
 *        module faulted.
 *
 * Breakpad can only symbolize addresses that belong to a loaded module. The
 * SourcePawn VM runs plugin code from JIT regions that belong to no module, and
 * its call frames live in the VM's own InvokeFrame list rather than in machine
 * stack frames, so a minidump alone can never name the plugin that triggered a
 * crash. That information only exists inside the dying process, which is why it
 * has to be collected from the crash callback.
 */

#ifndef _INCLUDE_ACCELERATOR_SP_STACK_H_
#define _INCLUDE_ACCELERATOR_SP_STACK_H_

#include <stddef.h>
#include "sp_vm_api.h"

namespace spstack {

using SourcePawn::IPluginContext;
using SourcePawn::IFrameIterator;

/**
 * @brief Simple fixed-capacity text sink with an async-safe formatter.
 *
 * Deliberately avoids printf and any allocator so the same code can run from a
 * signal handler and from a SourcePawn native.
 */
struct Output {
	char* buffer;
	size_t capacity;
	size_t length;
	bool truncated;

	Output(char* b, size_t cap) : buffer(b), capacity(cap), length(0), truncated(false) {
		if (cap)
			buffer[0] = '\0';
	}
};

/// Appends a NUL terminated string; NULL prints as "(null)".
void PutString(Output& out, const char* text);

/// Appends a printf-like subset: %s %d %u %x %p %c (no widths or modifiers).
void PutFormatted(Output& out, const char* fmt, ...);

/**
 * @brief Registers a plugin so its context can be named in reports.
 *
 * Called from the main thread while the process is healthy.
 *
 * @param context		Base context of the loaded plugin.
 * @param filename		Plugin filename, eg. "foo.smx".
 * @param hasDebugInfo	Whether the .smx carries .dbg lines/files, which is
 *						what makes file:line resolution possible.
 */
void RegisterPlugin(IPluginContext* context, const char* filename, bool hasDebugInfo);

/**
 * @brief Forgets a plugin that is going away.
 *
 * @param context		Base context of the unloaded plugin.
 */
void UnregisterPlugin(IPluginContext* context);

/**
 * @brief Records the thread that the VM runs on, for crash-time caveats.
 */
void InitOnMainThread();

/**
 * @brief Walks the live SourcePawn stack and writes human readable frames.
 *
 * @param out			Output buffer descriptor, updated in place.
 * @param maxFrames		Maximum number of frames to write.
 * @param preferred		Context to ask the VM with. Any live context works (the
 *						iterator reads the VM's global invoke stack), but the
 *						calling context is used when one is known.
 * @return				Number of frames written, or -1 if the VM could not be
 *						asked for a stack at all.
 */
int WriteStackFrames(Output& out, int maxFrames, IPluginContext* preferred = nullptr);

/**
 * @brief Builds the full "SOURCEPAWN STACK" report section.
 *
 * Intended to be called from a crash handler after the minidump is on disk.
 *
 * @param buffer		Caller owned buffer; must be static storage, not a
 *						stack array (the Linux handler runs on a small alt stack).
 * @param capacity		Size of buffer.
 * @param vmVersion		SourcePawn/SourceMod version string, may be NULL.
 * @return				Number of bytes written, excluding the NUL terminator.
 */
size_t WriteCrashReport(char* buffer, size_t capacity, const char* vmVersion);

/**
 * @brief True while a crash-time walk is in progress.
 *
 * Used by the Windows vectored exception handler so a fault raised by the walk
 * itself cannot trigger a second minidump.
 */
bool IsWalking();

} // namespace spstack

#endif // _INCLUDE_ACCELERATOR_SP_STACK_H_
