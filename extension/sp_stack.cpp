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

// Only the SourcePawn VM API is needed here, not SourceMod's extension glue,
// so this file can also be built into the standalone host under tests/.
#include "sp_stack.h"

#include <stdarg.h>
#include <string.h>
#include <map>
#include <string>

#if defined _LINUX
#include <signal.h>
#include "third_party/lss/linux_syscall_support.h"
#elif defined _WINDOWS
#include <windows.h>
#endif

namespace spstack {

// ----------------------------------------------------------------------------
// Plugin registry. Filled on the main thread while the process is healthy, so
// the crash path only ever reads it.
// ----------------------------------------------------------------------------

struct PluginInfo {
	std::string filename;
	bool hasDebugInfo;
};

static std::map<IPluginContext*, PluginInfo> s_plugins;
static unsigned long s_vmThreadId = 0;
static volatile bool s_walking = false;

static const int kMaxFrames = 64;
static const char* const kNoName = "(unregistered plugin)";

void RegisterPlugin(IPluginContext* context, const char* filename, bool hasDebugInfo)
{
	if (!context) {
		return;
	}

	PluginInfo info;
	info.filename = filename ? filename : kNoName;
	info.hasDebugInfo = hasDebugInfo;
	s_plugins[context] = info;
}

void UnregisterPlugin(IPluginContext* context)
{
	s_plugins.erase(context);
}

static const PluginInfo* FindPlugin(IPluginContext* context)
{
	if (!context) {
		return nullptr;
	}

	auto it = s_plugins.find(context);
	if (it == s_plugins.end()) {
		return nullptr;
	}

	return &it->second;
}

// Any live context will do: sp's FrameIterator starts at the environment's
// top-most invoke frame, not at the context it was created from.
static IPluginContext* PickContext()
{
	if (s_plugins.empty()) {
		return nullptr;
	}

	return s_plugins.begin()->first;
}

void InitOnMainThread()
{
#if defined _LINUX
	s_vmThreadId = static_cast<unsigned long>(sys_gettid());
#elif defined _WINDOWS
	s_vmThreadId = static_cast<unsigned long>(GetCurrentThreadId());
#else
	s_vmThreadId = 0;
#endif
}

bool IsWalking()
{
	return s_walking;
}

// ----------------------------------------------------------------------------
// Async-safe text output. No printf, no allocator, no locale: this runs from a
// signal handler and from a vectored exception handler.
// ----------------------------------------------------------------------------

static size_t TextLength(const char* text)
{
	const char* cursor = text;
	while (*cursor) {
		cursor++;
	}
	return static_cast<size_t>(cursor - text);
}

static void PutBytes(Output& out, const char* data, size_t length)
{
	if (length == 0) {
		return;
	}

	// Bytes are always reserved for the terminator, so the usable span is
	// capacity-1. Room is computed as its own value because a buffer this small
	// must not underflow the subtraction.
	size_t room = 0;
	if (out.capacity >= 2 && out.length + 1 < out.capacity) {
		room = out.capacity - 1 - out.length;
	}

	// Anything that did not fit is a truncation, including the case where the
	// buffer was already full and nothing at all could be written. Reporting
	// only the partial writes would leave whole lines silently missing.
	if (length > room) {
		out.truncated = true;
	}

	if (room == 0) {
		return;
	}

	if (length > room) {
		length = room;
	}

	for (size_t i = 0; i < length; ++i) {
		out.buffer[out.length + i] = data[i];
	}
	out.length += length;
	out.buffer[out.length] = '\0';
}

void PutString(Output& out, const char* text)
{
	if (!text) {
		text = "(null)";
	}
	PutBytes(out, text, TextLength(text));
}

static void PutUnsigned(Output& out, unsigned long long value)
{
	char digits[24];
	size_t count = 0;

	do {
		digits[count++] = static_cast<char>('0' + (value % 10));
		value /= 10;
	} while (value != 0 && count < sizeof(digits));

	for (size_t i = 0; i < count / 2; ++i) {
		char swap = digits[i];
		digits[i] = digits[count - 1 - i];
		digits[count - 1 - i] = swap;
	}

	PutBytes(out, digits, count);
}

static void PutSigned(Output& out, long long value)
{
	if (value < 0) {
		PutBytes(out, "-", 1);
		PutUnsigned(out, static_cast<unsigned long long>(-value));
		return;
	}

	PutUnsigned(out, static_cast<unsigned long long>(value));
}

static void PutHex(Output& out, unsigned long long value)
{
	static const char* const digits = "0123456789abcdef";
	char buffer[24];
	size_t count = 0;

	do {
		buffer[count++] = digits[value & 0xF];
		value >>= 4;
	} while (value != 0 && count < sizeof(buffer));

	PutBytes(out, "0x", 2);
	for (size_t i = 0; i < count / 2; ++i) {
		char swap = buffer[i];
		buffer[i] = buffer[count - 1 - i];
		buffer[count - 1 - i] = swap;
	}
	PutBytes(out, buffer, count);
}

void PutFormatted(Output& out, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	const char* cursor = fmt;
	while (*cursor) {
		if (*cursor != '%') {
			PutBytes(out, cursor, 1);
			cursor++;
			continue;
		}

		cursor++;
		switch (*cursor) {
			case 's': {
				const char* value = va_arg(ap, const char*);
				if (!value) {
					value = "(null)";
				}
				PutString(out, value);
				break;
			}
			case 'd':
				PutSigned(out, va_arg(ap, int));
				break;
			case 'u':
				PutUnsigned(out, va_arg(ap, unsigned int));
				break;
			case 'p':
				PutHex(out, reinterpret_cast<unsigned long long>(va_arg(ap, void*)));
				break;
			case 'x':
				PutHex(out, va_arg(ap, unsigned int));
				break;
			case 'c': {
				char ch = static_cast<char>(va_arg(ap, int));
				PutBytes(out, &ch, 1);
				break;
			}
			case '%':
				PutBytes(out, "%", 1);
				break;
			case '\0':
				// Trailing '%': emit it literally and stop.
				PutBytes(out, "%", 1);
				va_end(ap);
				return;
			default:
				// Unknown specifier: emit it verbatim rather than misreading
				// the argument list.
				PutBytes(out, "%", 1);
				PutBytes(out, cursor, 1);
				break;
		}

		if (*cursor) {
			cursor++;
		}
	}

	va_end(ap);
}

// ----------------------------------------------------------------------------
// Stack walk
// ----------------------------------------------------------------------------

// "Which plugin functions were running" does not need a second walk. The VM keeps
// one invoke chain for the whole process - FrameIterator starts at
// Environment::get()->top() (sourcepawn vm/stack-frames.cpp) and every frame
// carries its own plugin - so a single walk already crosses plugin boundaries.
// What this adds is the grouping: frames are also collected per plugin, so the
// answer is one line per plugin instead of something to reconstruct by eye.
struct RunningEntry
{
	const char *plugin;
	int frames;
	size_t length;
	bool truncated;
	char functions[320];
};

static const int kMaxRunningPlugins = 24;
static RunningEntry s_running[kMaxRunningPlugins];
static int s_runningCount;
static int s_runningOverflow;

static void ResetRunning()
{
	s_runningCount = 0;
	s_runningOverflow = 0;
}

static void RecordRunning(const char *pluginName, const char *functionName)
{
	RunningEntry *entry = nullptr;
	for (int i = 0; i < s_runningCount; ++i) {
		// The plugin name is normally the same pointer every time (the string lives
		// in the registry), so this is usually a pointer compare.
		if (s_running[i].plugin == pluginName || strcmp(s_running[i].plugin, pluginName) == 0) {
			entry = &s_running[i];
			break;
		}
	}

	if (!entry) {
		if (s_runningCount >= kMaxRunningPlugins) {
			s_runningOverflow++;
			return;
		}
		entry = &s_running[s_runningCount++];
		entry->plugin = pluginName;
		entry->frames = 0;
		entry->length = 0;
		entry->truncated = false;
		entry->functions[0] = '\0';
	}

	entry->frames++;

	if (!functionName) {
		functionName = "(null)";
	}

	const char *pieces[2] = { entry->length ? " <- " : "", functionName };
	for (int p = 0; p < 2; ++p) {
		for (const char *cursor = pieces[p]; *cursor; ++cursor) {
			if (entry->length + 1 >= sizeof(entry->functions)) {
				entry->truncated = true;
				break;
			}
			entry->functions[entry->length++] = *cursor;
			entry->functions[entry->length] = '\0';
		}
		if (entry->truncated) {
			break;
		}
	}
}

int WriteStackFrames(Output& out, int maxFrames, IPluginContext* preferred)
{
	if (maxFrames <= 0 || maxFrames > kMaxFrames) {
		maxFrames = kMaxFrames;
	}

	ResetRunning();

	IPluginContext* context = preferred ? preferred : PickContext();
	if (!context) {
		PutFormatted(out, "Status=no-plugin-contexts\n"
			"Note=Accelerator has no loaded plugin contexts to query, so the "
			"SourcePawn stack could not be read.\n");
		return -1;
	}

	IFrameIterator* iter = context->CreateFrameIterator();
	if (!iter) {
		PutFormatted(out, "Status=iterator-unavailable\n"
			"Note=The SourcePawn VM refused to hand out a frame iterator.\n");
		return -1;
	}

	int index = 0;
	int written = 0;
	for (; !iter->Done() && index < maxFrames; iter->Next(), ++index) {
		IPluginContext* frameContext = iter->Context();
		const PluginInfo* plugin = FindPlugin(frameContext);
		const char* pluginName = plugin ? plugin->filename.c_str() : kNoName;

		if (iter->IsNativeFrame()) {
			PutFormatted(out, "[%d] native   \"%s\"  called-by=%s\n",
				index,
				iter->FunctionName(),
				pluginName);
			written++;
			continue;
		}

		if (iter->IsScriptedFrame()) {
			PutFormatted(out, "[%d] script   \"%s\"  %s:%u  plugin=%s",
				index,
				iter->FunctionName(),
				iter->FilePath() ? iter->FilePath() : "(unknown file)",
				iter->LineNumber(),
				pluginName);
			if (iter->LineNumber() == 0 && plugin && !plugin->hasDebugInfo) {
				PutString(out, "  note=plugin compiled without debug info");
			}
			PutString(out, "\n");
			RecordRunning(pluginName, iter->FunctionName());
			written++;
			continue;
		}

		// Internal frames (VM helpers) carry no source position; they are still
		// listed so the frame numbering matches what a debugger would show.
		PutFormatted(out, "[%d] internal (%s)\n", index, pluginName);
		written++;
	}

	bool exhausted = iter->Done();
	context->DestroyFrameIterator(iter);

	if (!exhausted) {
		PutFormatted(out, "Truncated=true (more frames beyond %d)\n", maxFrames);
	}

	if (s_runningCount > 0) {
		PutFormatted(out, "Running-Plugins=%d", s_runningCount);
		if (s_runningOverflow) {
			PutFormatted(out, " (+%d more not listed)", s_runningOverflow);
		}
		PutString(out, "\n");

		for (int i = 0; i < s_runningCount; ++i) {
			PutFormatted(out, "plugin=%s frames=%d functions=\"%s\"%s\n",
				s_running[i].plugin,
				s_running[i].frames,
				s_running[i].functions,
				s_running[i].truncated ? " (truncated)" : "");
		}
	}

	return written;
}

// ----------------------------------------------------------------------------
// Crash-time guards
// ----------------------------------------------------------------------------
//
// The walk happens after the minidump is on disk, so the worst case is losing
// the new section. What must not happen is re-entering breakpad, or hanging
// forever: SourcePawn's CreateFrameIterator() calls operator new, and malloc
// may be holding a lock if the crash happened inside the allocator.

#if defined _LINUX

static const unsigned kWalkWatchdogSeconds = 10;

class LinuxWalkGuard
{
	static const int kSignalCount = 5;
	int m_Signals[kSignalCount];
	struct sigaction m_Saved[kSignalCount];
	bool m_armed;

public:
	LinuxWalkGuard() : m_armed(false)
	{
		m_Signals[0] = SIGSEGV;
		m_Signals[1] = SIGBUS;
		m_Signals[2] = SIGILL;
		m_Signals[3] = SIGFPE;
		m_Signals[4] = SIGABRT;

		for (int i = 0; i < kSignalCount; ++i) {
			struct sigaction act;
			memset(&act, 0, sizeof(act));
			sigemptyset(&act.sa_mask);
			act.sa_handler = SIG_DFL;
			sigaction(m_Signals[i], &act, &m_Saved[i]);
		}

		// A hang here is worse than dying: the dump is already written, so let
		// the default SIGALRM action end the process instead of freezing the
		// server on a malloc lock.
		struct sigaction alarmAct;
		memset(&alarmAct, 0, sizeof(alarmAct));
		sigemptyset(&alarmAct.sa_mask);
		alarmAct.sa_handler = SIG_DFL;
		sigaction(SIGALRM, &alarmAct, nullptr);
		alarm(kWalkWatchdogSeconds);

		m_armed = true;
		s_walking = true;
	}

	~LinuxWalkGuard()
	{
		s_walking = false;
		if (!m_armed) {
			return;
		}

		alarm(0);
		for (int i = 0; i < kSignalCount; ++i) {
			sigaction(m_Signals[i], &m_Saved[i], nullptr);
		}
	}

private:
	LinuxWalkGuard(const LinuxWalkGuard&);
	LinuxWalkGuard& operator=(const LinuxWalkGuard&);
};

static int WalkUnderGuard(Output& out, int maxFrames)
{
	LinuxWalkGuard guard;
	return WriteStackFrames(out, maxFrames);
}

#elif defined _WINDOWS

// Returns false if the walk itself raised an exception. The exception is
// swallowed here, and BreakpadVectoredHandler() checks IsWalking() so it will
// not try to write a second minidump for it.
static bool WalkUnderGuard(Output& out, int maxFrames, int* frames)
{
	s_walking = true;
	bool ok = true;

	__try {
		*frames = WriteStackFrames(out, maxFrames);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		ok = false;
	}

	s_walking = false;
	return ok;
}

#else

static int WalkUnderGuard(Output& out, int maxFrames)
{
	s_walking = true;
	int frames = WriteStackFrames(out, maxFrames);
	s_walking = false;
	return frames;
}

#endif

// ----------------------------------------------------------------------------
// Crash report section
// ----------------------------------------------------------------------------

// Static storage, not stack: on Linux this runs on breakpad's alternate signal
// stack, which is only a few tens of kilobytes. Sized for 64 frames plus the
// per-plugin summary; the caller's report buffer is 32 KB and notes take at most
// 4 KB, so this fits with room to spare.
static char s_frameBuffer[24576];
static char s_notes[4096];

static const char s_stackBegin[] = "-------- SOURCEPAWN STACK BEGIN --------\n";
static const char s_stackEnd[] = "-------- SOURCEPAWN STACK END --------\n";
static const char s_truncatedTail[] = "Truncated=true\n";

size_t WriteCrashReport(char* buffer, size_t capacity, const char* vmVersion)
{
	// Hold back room for the closing lines. Without this, "Truncated=true"
	// would have to be written into a buffer that is by definition already full,
	// so it could never appear; and a report with no STACK END would be
	// ambiguous between "the walk died" and "the tail did not fit". With it, the
	// END marker is always the last line, and a Truncated line before it means
	// only that some of the middle did not fit.
	const size_t reserve = TextLength(s_stackEnd) + TextLength(s_truncatedTail);
	if (!buffer || capacity <= reserve) {
		return 0;
	}

	Output out(buffer, capacity - reserve);

	// The frames are collected into their own buffer first so the report can
	// state how many there are, and so a truncated or faulted walk still shows
	// up as its own reading instead of silently shrinking the trace.
	Output frames(s_frameBuffer, sizeof(s_frameBuffer));
	int frameCount = -1;
	bool walkFaulted = false;

#if defined _WINDOWS
	int walked = -1;
	if (WalkUnderGuard(frames, kMaxFrames, &walked)) {
		frameCount = walked;
	} else {
		walkFaulted = true;
	}
#else
	frameCount = WalkUnderGuard(frames, kMaxFrames);
#endif

	Output notes(s_notes, sizeof(s_notes));
	unsigned long vmThreadId = s_vmThreadId;
	unsigned long crashThreadId = 0;
#if defined _LINUX
	crashThreadId = static_cast<unsigned long>(sys_gettid());
#elif defined _WINDOWS
	crashThreadId = static_cast<unsigned long>(GetCurrentThreadId());
#endif

	if (walkFaulted) {
		PutString(notes,
			"Status=walk-faulted\n"
			"Note=reading the SourcePawn stack raised a second exception while "
			"the process was already dying; the minidump on disk is still valid\n");
	}
	if (vmThreadId != 0 && crashThreadId != 0 && vmThreadId != crashThreadId) {
		PutString(notes,
			"Caveat=the crash happened on a different thread than the one that "
			"loaded SourceMod; the frames below are the VM's own invoke stack, "
			"which is not necessarily what the crashing thread was doing\n");
	}
	if (frameCount == 0 && !walkFaulted) {
		// An empty trace is a result, not a missing measurement.
		PutString(notes,
			"Status=vm-idle\n"
			"Note=no plugin was executing, so this crash has no SourcePawn call "
			"stack to blame\n");
	}
	if (frames.truncated) {
		PutString(notes, "Truncated=true (frame buffer full)\n");
	}

	PutString(out, s_stackBegin);
	PutFormatted(out, "VM-Version=%s\n", vmVersion ? vmVersion : "(unknown)");
	PutFormatted(out, "VM-Thread=%u Crashed-Thread=%u\n", vmThreadId, crashThreadId);
	if (frameCount >= 0) {
		PutFormatted(out, "Frames=%d\n", frameCount);
	} else {
		// Keep "no frames" distinguishable from "could not count frames".
		PutString(out, "Frames=? (the walk could not count frames)\n");
	}
	if (notes.length) {
		PutBytes(out, s_notes, notes.length);
	}
	if (frames.length) {
		PutBytes(out, s_frameBuffer, frames.length);
	}
	PutString(out,
		"Legend=native=engine/extension function, script=plugin code with "
		"file:line, internal=VM helper frame\n");

	// The held-back room goes last: the truncation note first, then the closing
	// marker, so the marker is always the final line of a well-formed section.
	Output tail(buffer + out.length, capacity - out.length);
	if (out.truncated) {
		PutString(tail, s_truncatedTail);
	}
	PutString(tail, s_stackEnd);

	return out.length + tail.length;
}

} // namespace spstack
