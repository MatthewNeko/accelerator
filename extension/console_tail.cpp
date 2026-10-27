#include "console_tail.h"

namespace consoletail
{
	static int CountLines(const char *text, size_t length)
	{
		if (length == 0) {
			return 0;
		}

		int lines = 0;
		for (size_t i = 0; i < length; ++i) {
			if (text[i] == '\n') {
				lines++;
			}
		}

		// A buffer that does not end in a newline still has a final line.
		if (text[length - 1] != '\n') {
			lines++;
		}
		return lines;
	}

	Range Last(const char *text, size_t length, int want)
	{
		Range range;
		range.start = 0;
		range.length = length;
		range.total = CountLines(text, length);
		range.kept = range.total;

		if (!text || length == 0 || want <= 0 || range.total <= want) {
			return range;
		}

		int skip = range.total - want;
		size_t offset = 0;
		int seen = 0;
		while (offset < length && seen < skip) {
			if (text[offset] == '\n') {
				seen++;
			}
			offset++;
		}

		range.start = offset;
		range.length = length - offset;
		range.kept = range.total - skip;
		return range;
	}
}
