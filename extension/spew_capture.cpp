/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * =============================================================================
 * Accelerator - SourcePawn stack capture
 * =============================================================================
 *
 * Own console ring, fed by tier0's logging system. See spew_capture.h for why
 * the engine's GetSpew cannot be used on CS:GO Legacy.
 */

#include "spew_capture.h"

#if defined _LINUX
#include <dlfcn.h>
#include <link.h>
#include <string.h>
#elif defined _WINDOWS
#include <windows.h>
#else
#error Bad platform.
#endif

namespace spewcapture {
namespace {

// Large enough for a burst of plugin errors, small enough to stay out of the
// way: 128 KB of BSS, scanned only when a crash happens.
const size_t kRingSize = 128 * 1024;

// A very long line is kept from its end: that is where the message is.
char s_ring[kRingSize];
volatile unsigned s_head = 0;		// Next write offset.
volatile bool s_wrapped = false;
volatile long s_lock = 0;
volatile bool s_installed = false;
const char *s_failure = "";

typedef void (*ListenerCall_t)(void *);
typedef void (*RegisterListener_t)(void *);

ListenerCall_t s_unregister = NULL;

// Same layout as tier0's ILoggingListener: one virtual, no destructor. The
// parameter types do not participate in the ABI, so LoggingContext_t and tchar
// stay opaque here and the extension needs no tier0 headers or link library.
class LoggingListener
{
public:
	virtual void Log(const void *context, const char *message) = 0;
};

char At(size_t offset)
{
	return s_ring[offset % kRingSize];
}

#if defined _WINDOWS
void LockRing()
{
	while (InterlockedExchange(&s_lock, 1) != 0) {
		Sleep(0);
	}
}

void UnlockRing()
{
	InterlockedExchange(&s_lock, 0);
}
#else
void LockRing()
{
	while (__sync_lock_test_and_set(&s_lock, 1)) {
		while (s_lock) {
		}
	}
}

void UnlockRing()
{
	__sync_lock_release(&s_lock);
}
#endif

class ConsoleListener : public LoggingListener
{
public:
	void Log(const void *context, const char *message)
	{
		(void)context;
		Record(message);
	}
};

ConsoleListener s_listener;

#if defined _LINUX
// tier0 is loaded by the engine with local visibility, so RTLD_DEFAULT does not
// see its symbols (measured: dlsym(RTLD_DEFAULT) returns NULL on a live
// server). Find the already-loaded object and search its own handle instead.
char s_tier0PathBuffer[512];
int FindTier0(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	if (!info->dlpi_name || !info->dlpi_name[0]) {
		return 0;	// The main executable has no name; tier0 is never it.
	}
	if (!strstr(info->dlpi_name, "libtier0")) {
		return 0;
	}
	size_t length = 0;
	while (info->dlpi_name[length] && length + 1 < sizeof(s_tier0PathBuffer)) {
		s_tier0PathBuffer[length] = info->dlpi_name[length];
		++length;
	}
	s_tier0PathBuffer[length] = '\0';
	*(const char **)data = s_tier0PathBuffer;
	return 1;	// Stop iterating.
}

void *OpenTier0()
{
	const char *path = NULL;
	dl_iterate_phdr(FindTier0, &path);

	void *handle = NULL;
	if (path) {
		handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
	}
	if (!handle) {
		handle = dlopen("libtier0.so", RTLD_NOW | RTLD_NOLOAD);
	}
	if (!handle) {
		handle = dlopen("libtier0.so", RTLD_NOW);
	}
	return handle;
}
#endif

} // anonymous namespace

void Record(const char *message)
{
	if (!message || !message[0]) {
		return;
	}

	size_t length = 0;
	while (message[length]) {
		++length;
	}
	if (!length) {
		return;
	}

	const char *source = message;
	if (length > kRingSize) {
		source += length - kRingSize;
		length = kRingSize;
	}

	LockRing();
	unsigned head = s_head;
	for (size_t i = 0; i < length; ++i) {
		s_ring[head] = source[i];
		if (++head == kRingSize) {
			head = 0;
			s_wrapped = true;
		}
	}
	s_head = head;
	UnlockRing();
}

bool Wrapped()
{
	return s_wrapped;
}

bool Install()
{
	if (s_installed) {
		return true;
	}

	void *reg = NULL;
	void *unreg = NULL;
#if defined _LINUX
	void *tier0 = OpenTier0();
	if (tier0) {
		reg = dlsym(tier0, "LoggingSystem_RegisterLoggingListener");
		unreg = dlsym(tier0, "LoggingSystem_UnregisterLoggingListener");
	}
	if (!reg) {
		// Fall back to the global namespace for engines that load tier0 globally.
		reg = dlsym(RTLD_DEFAULT, "LoggingSystem_RegisterLoggingListener");
		unreg = dlsym(RTLD_DEFAULT, "LoggingSystem_UnregisterLoggingListener");
	}
#else
	HMODULE tier0 = GetModuleHandleA("tier0.dll");
	if (!tier0) {
		tier0 = LoadLibraryA("tier0.dll");
	}
	if (tier0) {
		reg = (void *)GetProcAddress(tier0, "LoggingSystem_RegisterLoggingListener");
		unreg = (void *)GetProcAddress(tier0, "LoggingSystem_UnregisterLoggingListener");
	}
#endif

	if (!reg) {
		s_failure = "the engine does not export LoggingSystem_RegisterLoggingListener";
		return false;
	}
	// Without the matching unregister call the engine would keep a pointer
	// into this module after a sm exts unload unmapped it. Prefer GetSpew.
	if (!unreg) {
		s_failure = "the engine exports a logging listener but no way to unregister it";
		return false;
	}

	s_unregister = reinterpret_cast<ListenerCall_t>(unreg);
	reinterpret_cast<RegisterListener_t>(reg)(&s_listener);
	s_installed = true;
	return true;
}

void Uninstall()
{
	if (!s_installed) {
		return;
	}
	if (s_unregister) {
		s_unregister(&s_listener);
		s_unregister = NULL;
	}
	s_installed = false;
}

bool Installed()
{
	return s_installed;
}

const char *FailureReason()
{
	return s_failure;
}

size_t CopyTail(char *dst, size_t capacity, int maxLines, Tail *tail)
{
	if (tail) {
		tail->kept = 0;
		tail->held = 0;
		tail->cut = false;
	}
	if (!dst || capacity < 2) {
		return 0;
	}
	dst[0] = '\0';

	// Snapshot: head, the wrap flag and the ring itself are written by other
	// threads. A torn tail is possible if the crash landed mid-write; the seam
	// trim below is what makes that harmless.
	const unsigned head = s_head;
	const bool wrapped = s_wrapped;
	size_t valid = wrapped ? kRingSize : head;
	if (!valid) {
		return 0;
	}

	unsigned begin = wrapped ? head : 0;

	// The oldest byte of a wrapped ring is the middle of a line. Start after
	// that newline: half a line at the top would read like corruption. If the
	// ring holds one unfinished line and nothing else, keep the fragment - an
	// empty section would be the bigger lie.
	if (wrapped) {
		size_t skip = 0;
		while (skip < valid && At(begin + skip) != '\n') {
			++skip;
		}
		if (skip < valid) {
			begin = (begin + skip + 1) % kRingSize;
			valid -= skip + 1;
			if (!valid) {
				return 0;
			}
		}
	}

	// How many lines the ring is holding, for the "Lines=kept/held" header.
	int held = 1;
	for (size_t i = 0; i < valid; ++i) {
		if (At(begin + i) == '\n') {
			++held;
		}
	}
	if (At(begin + valid - 1) == '\n') {
		--held;
	}

	size_t length = valid;
	if (maxLines > 0) {
		// Walk backwards until the (maxLines+1)th newline; keep what follows.
		int newlines = 0;
		for (size_t i = 0; i < valid; ++i) {
			size_t pos = (begin + valid - 1 - i) % kRingSize;
			if (s_ring[pos] == '\n') {
				if (newlines == maxLines) {
					begin = (unsigned)((pos + 1) % kRingSize);
					length = i;
					break;
				}
				++newlines;
			}
		}
	}

	// Capacity trim keeps the newest bytes, but starts at a line boundary so a
	// half sentence at the top is not mistaken for the start of a line.
	const size_t limit = capacity - 1;
	if (length > limit) {
		size_t excess = length - limit;
		size_t boundary = length;
		for (size_t i = excess - 1; i < length; ++i) {
			if (At(begin + i) == '\n') {
				boundary = i;
				break;
			}
		}
		if (boundary + 1 < length) {
			size_t skip = boundary + 1;
			begin = (unsigned)((begin + skip) % kRingSize);
			length -= skip;
		} else {
			// One line longer than the whole destination: keep its tail.
			begin = (unsigned)((begin + excess) % kRingSize);
			length = limit;
		}
	}

	size_t written = 0;
	for (size_t i = 0; i < length; ++i) {
		dst[written++] = At(begin + i);
	}
	dst[written] = '\0';

	if (tail) {
		int kept = 0;
		for (size_t i = 0; i < written; ++i) {
			if (dst[i] == '\n') {
				++kept;
			}
		}
		if (written && dst[written - 1] != '\n') {
			++kept;
		}
		tail->kept = kept;
		tail->held = held;
		tail->cut = kept < held;
	}

	return written;
}

} // namespace spewcapture

#if defined ACCELERATOR_SPEW_CAPTURE_TEST
namespace spewcapture {

void TestReset()
{
	s_head = 0;
	s_wrapped = false;
	s_lock = 0;
}

} // namespace spewcapture
#endif
