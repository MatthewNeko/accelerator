/**
 * Keeps the tail of a console-history buffer.
 *
 * The engine hands back up to 64 KB of history. A crash report only needs the
 * last few hundred lines, and shipping the whole buffer makes the file harder to
 * scan than it needs to be. This runs from a signal handler, so it is a pair of
 * linear scans over the buffer: no allocation, no stdio, no locale, and no
 * assumption that the buffer ends with a newline.
 */
#ifndef _INCLUDE_ACCELERATOR_CONSOLE_TAIL_H_
#define _INCLUDE_ACCELERATOR_CONSOLE_TAIL_H_

#include <stddef.h>

namespace consoletail
{
	struct Range
	{
		size_t start;   // offset of the first kept byte
		size_t length;  // bytes kept
		int total;      // lines in the whole buffer
		int kept;       // lines in [start, start+length)
	};

	/**
	 * @brief The last |want| lines of |text|.
	 *
	 * A trailing newline does not start a new line, so "a\nb\n" is two lines and
	 * returns everything for want >= 2. A buffer without a trailing newline counts
	 * its final partial line. want <= 0 means "keep everything" and is reported as
	 * such in the result.
	 */
	Range Last(const char *text, size_t length, int want);
}

#endif
