/**
 * vim: set ts=4 sw=4 tw=99 noet :
 * =============================================================================
 * Accelerator - SourcePawn stack capture
 * =============================================================================
 *
 * Console history for engine builds that no longer fill GetSpew's buffer.
 *
 * CS:GO Legacy's engine.so still contains GetSpew, and the csgo gamedata
 * section still finds it with a byte signature, but the linked list it walks
 * is initialised empty and never appended to: the only writes to its head and
 * tail indices in the whole engine binary are the constructor and GetSpew's
 * own reads. The engine's own "Console History (reversed)" dump code is gone
 * too - the literal survives, but nothing references it. So on those builds
 * GetSpew always returns "" and a report can only say "console-empty".
 *
 * The logging system did survive. tier0 exports
 * LoggingSystem_RegisterLoggingListener (the engine itself calls it), and every
 * console line goes through it, so the extension keeps its own ring of recent
 * output and the crash callback reads it. This is the same sink SourceMod's
 * sm_conhook uses on CS:GO.
 */

#ifndef _INCLUDE_ACCELERATOR_SPEW_CAPTURE_H_
#define _INCLUDE_ACCELERATOR_SPEW_CAPTURE_H_

#include <stddef.h>

namespace spewcapture {

/**
 * @brief Registers the logging listener.
 *
 * Resolved at runtime, never linked: the entry point lives in tier0, and an
 * engine without it must degrade to GetSpew instead of failing to load.
 *
 * @return	True when the listener is registered.
 */
bool Install();

/**
 * @brief Removes the listener before the extension's code is unmapped.
 *
 * Uses LoggingSystem_UnregisterLoggingListener when tier0 exports it; without
 * it the engine keeps a pointer into this module, so Install() is only safe on
 * engines that can also unregister.
 */
void Uninstall();

/** @brief Whether the listener is currently registered. */
bool Installed();

/** @brief Why capture is not running, as a static literal; "" when running. */
const char *FailureReason();

/** @brief Appends one engine log message. Called on whatever thread logged it. */
void Record(const char *message);

struct Tail
{
	int kept;		/**< Lines written to the destination. */
	int held;		/**< Lines the ring held before the tail cut. */
	bool cut;		/**< True when older lines were dropped. */
};

/**
 * @brief Copies the newest lines of captured output into a buffer.
 *
 * Safe to call from the crash callback: no allocation, no locks, no libc
 * calls. A partial line at the ring's seam, or one split by the capacity, is
 * dropped rather than copied in half.
 *
 * @param dst			Destination buffer.
 * @param capacity		Size of the destination, including the terminator.
 * @param maxLines		Keep at most this many lines; <= 0 keeps everything that fits.
 * @param tail			Optional; receives the line counts.
 * @return				Bytes written, excluding the terminator.
 */
size_t CopyTail(char *dst, size_t capacity, int maxLines, Tail *tail);

/** @brief Whether the ring has overwritten anything (the oldest bytes are gone). */
bool Wrapped();

#if defined ACCELERATOR_SPEW_CAPTURE_TEST
/** @brief Empties the ring; only built for the unit tests. */
void TestReset();
#endif

} // namespace spewcapture

#endif // _INCLUDE_ACCELERATOR_SPEW_CAPTURE_H_
