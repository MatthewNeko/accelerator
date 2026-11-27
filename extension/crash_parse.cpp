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

#include "crash_parse.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <map>
#include <sstream>
#include <streambuf>

#include <google_breakpad/processor/call_stack.h>
#include <google_breakpad/processor/code_module.h>
#include <google_breakpad/processor/code_modules.h>
#include <google_breakpad/processor/minidump.h>
#include <google_breakpad/processor/minidump_processor.h>
#include <google_breakpad/processor/process_state.h>
#include <google_breakpad/processor/stack_frame.h>
#include <processor/pathname_stripper.h>

#if defined _LINUX
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace crashparse {

namespace {

/**
 * @brief The version of the summary line layout the crash server expects.
 *
 * Fields are appended over time, so this has to match the field list below or
 * the server misreads the signature.
 */
const int kSummaryFormatVersion = 2;

/** @brief How much of a stack the summary reports; deeper stacks are noise. */
const int kMaxSummaryFrames = 1024;

/** @brief Every minidump starts with this; MINIDUMP_SIGNATURE as bytes. */
const char kMinidumpMagic[4] = {'M', 'D', 'M', 'P'};

/** @brief Size of MINIDUMP_HEADER. */
const uint32_t kMinidumpHeaderSize = 32;

/** @brief Size of MINIDUMP_DIRECTORY. */
const uint32_t kMinidumpDirectorySize = 12;

/** @brief Stream types this file cares about, from MINIDUMP_STREAM_TYPE. */
enum StreamType
{
	kStreamThreadList = 3,
	kStreamModuleList = 4,
};

/**
 * @brief Thread count above which a dump is treated as damaged.
 *
 * A source engine server does not have thousands of threads; a thread list that
 * says it does has been written over a torn-down structure, and walking it is
 * how the parser used to die.
 */
const uint32_t kMaxPlausibleThreads = 4096;

/**
 * @brief Reads a little-endian uint32 without depending on host layout.
 *
 * A minidump is data, so its byte order is decoded rather than assumed.
 */
uint32_t ReadUInt32LE(const unsigned char *bytes)
{
	return static_cast<uint32_t>(bytes[0])
		| (static_cast<uint32_t>(bytes[1]) << 8)
		| (static_cast<uint32_t>(bytes[2]) << 16)
		| (static_cast<uint32_t>(bytes[3]) << 24);
}

/**
 * @brief Builds the crash signature and module table for a parsed dump.
 *
 * The signature is what the crash server's presubmit matches on: a header, one
 * |M| record per module so the server knows which symbols it lacks, and one |F|
 * record per frame of the crashing thread, addressed as an offset inside its
 * module. The module list is also kept in the same order, because the server
 * answers with one character per module.
 */
bool BuildSummary(google_breakpad::ProcessState &processState, ParseResult *result)
{
	const google_breakpad::CodeModules *modules = processState.modules();
	unsigned int moduleCount = modules ? modules->module_count() : 0;

	std::string osShort;
	std::string cpuArch;
	if (processState.system_info()) {
		osShort = processState.system_info()->os_short;
		if (osShort.empty()) {
			osShort = processState.system_info()->os;
		}
		cpuArch = processState.system_info()->cpu;
	}

	if (!processState.threads() || processState.threads()->empty()) {
		result->failure = "the minidump has no thread list";
		return false;
	}

	int requestingThread = processState.requesting_thread();
	if (requestingThread < 0
		|| static_cast<size_t>(requestingThread) >= processState.threads()->size()) {
		requestingThread = 0;
	}

	const google_breakpad::CallStack *stack = processState.threads()->at(
		static_cast<size_t>(requestingThread));
	if (!stack || !stack->frames()) {
		result->failure = "the minidump has no stack for the crashing thread";
		return false;
	}

	int frameCount = static_cast<int>(stack->frames()->size());
	if (frameCount > kMaxSummaryFrames) {
		frameCount = kMaxSummaryFrames;
	}

	std::ostringstream summaryStream;
	summaryStream << kSummaryFormatVersion
		<< "|" << processState.time_date_stamp()
		<< "|" << osShort
		<< "|" << cpuArch
		<< "|" << processState.crashed()
		<< "|" << processState.crash_reason()
		<< "|" << std::hex << processState.crash_address() << std::dec
		<< "|" << requestingThread;

	result->modules.clear();
	result->modules.reserve(moduleCount);

	std::map<const google_breakpad::CodeModule *, unsigned int> moduleIndexes;
	for (unsigned int moduleIndex = 0; moduleIndex < moduleCount; ++moduleIndex) {
		const google_breakpad::CodeModule *module = modules->GetModuleAtIndex(moduleIndex);
		if (!module) {
			// The summary is a positional list for the server, so a hole in it
			// would shift every module after it. Refuse rather than mislabel.
			result->failure = "the minidump has an unreadable module";
			return false;
		}

		moduleIndexes[module] = moduleIndex;

		ModuleInfo info;
		info.codeFile = module->code_file();
		info.debugFile = module->debug_file();
		info.debugIdentifier = module->debug_identifier();
		info.codeIdentifier = module->code_identifier();
		result->modules.push_back(info);

		summaryStream << "|M|"
			<< google_breakpad::PathnameStripper::File(info.debugFile)
			<< "|" << info.debugIdentifier;
	}

	for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
		const google_breakpad::StackFrame *frame = stack->frames()->at(
			static_cast<size_t>(frameIndex));
		if (!frame) {
			continue;
		}

		int moduleIndex = -1;
		uint64_t moduleOffset = frame->ReturnAddress();
		if (frame->module) {
			std::map<const google_breakpad::CodeModule *, unsigned int>::const_iterator it =
				moduleIndexes.find(frame->module);
			if (it != moduleIndexes.end()) {
				moduleIndex = static_cast<int>(it->second);
				moduleOffset -= frame->module->base_address();
			}
		}

		summaryStream << "|F|" << moduleIndex << "|" << std::hex << moduleOffset << std::dec;
	}

	result->summary = summaryStream.str();

	if (moduleCount) {
		const google_breakpad::CodeModule *mainModule = modules->GetMainModule();
		if (mainModule) {
			result->mainModuleCodeFile = mainModule->code_file();
		}
	}
	if (result->mainModuleCodeFile.empty() && !result->modules.empty()) {
		// Classification needs a directory even when the dump does not name a
		// main module; the lowest loaded module is the closest thing available.
		result->mainModuleCodeFile = result->modules[0].codeFile;
	}

	return true;
}

/**
 * @brief A stream buffer that discards everything written to it.
 *
 * breakpad reports its progress and its complaints on std::clog, which the game
 * server has no reader for. Pointing the stream at this instead of clearing its
 * buffer matters: writing through a stream whose rdbuf() is null is undefined
 * behaviour, and it faults on the very next logging line breakpad reaches.
 */
class NullStreambuf : public std::streambuf
{
protected:
	int overflow(int c) override { return traits_type::not_eof(c); }
	std::streamsize xsputn(const char *, std::streamsize count) override { return count; }
};

/**
 * @brief Sends std::clog to a bit bucket for the lifetime of the object.
 */
class ClogInhibitor
{
	std::streambuf *saved;
	NullStreambuf sink;

public:
	ClogInhibitor() {
		saved = std::clog.rdbuf(&sink);
	}

	~ClogInhibitor() {
		std::clog.rdbuf(saved);
	}
};

/**
 * @brief Parses the dump and fills in @a result.
 *
 * Runs wherever it is called, which on Linux means inside the forked child.
 */
bool ParseInPlace(const char *path, ParseResult *result)
{
	ClogInhibitor clogInhibitor;

	google_breakpad::ProcessState processState;
	google_breakpad::MinidumpProcessor minidumpProcessor(nullptr, nullptr);
	google_breakpad::ProcessResult processResult = minidumpProcessor.Process(path, &processState);

	if (processResult != google_breakpad::PROCESS_OK) {
		char reason[128];
		snprintf(reason, sizeof(reason), "breakpad could not process the minidump (result %d)",
			static_cast<int>(processResult));
		result->failure = reason;
		return false;
	}

	if (!BuildSummary(processState, result)) {
		return false;
	}

	result->parsed = true;
	return true;
}

#if defined _LINUX

/**
 * @brief How long a forked parse may take before it is abandoned.
 *
 * Parsing one dump is local file work measured in milliseconds; a child still
 * running after a minute is wedged (a fork while another thread held a lock, a
 * pathological file) and the upload thread cannot wait for it.
 */
const int kParseTimeoutSeconds = 30;

/** @brief Serialization magic values; a partial or foreign blob fails to read. */
const uint32_t kBlobMagic = 0x31444350;    /* "PCD1" */
const uint32_t kBlobEndMagic = 0x31454e44; /* "DNE1" */

/** @brief Large enough for any real dump's module table, small enough to hold. */
const size_t kMaxBlobSize = 4 * 1024 * 1024;

void AppendUInt32(std::string *out, uint32_t value)
{
	unsigned char bytes[4];
	bytes[0] = static_cast<unsigned char>(value & 0xff);
	bytes[1] = static_cast<unsigned char>((value >> 8) & 0xff);
	bytes[2] = static_cast<unsigned char>((value >> 16) & 0xff);
	bytes[3] = static_cast<unsigned char>((value >> 24) & 0xff);
	out->append(reinterpret_cast<const char *>(bytes), sizeof(bytes));
}

void AppendString(std::string *out, const std::string &value)
{
	AppendUInt32(out, static_cast<uint32_t>(value.size()));
	out->append(value);
}

/**
 * @brief Cursor over the blob the child wrote.
 *
 * Every read is bounds checked: the parent cannot tell a complete result from a
 * truncated one, and must not read past what the pipe actually delivered.
 */
class BlobReader
{
public:
	BlobReader(const char *data, size_t length) : data_(data), length_(length), offset_(0) {}

	bool UInt32(uint32_t *value) {
		if (length_ - offset_ < sizeof(uint32_t)) {
			return false;
		}
		*value = ReadUInt32LE(reinterpret_cast<const unsigned char *>(data_ + offset_));
		offset_ += sizeof(uint32_t);
		return true;
	}

	bool String(std::string *value) {
		uint32_t length = 0;
		if (!UInt32(&length) || length > kMaxBlobSize || length_ - offset_ < length) {
			return false;
		}
		value->assign(data_ + offset_, length);
		offset_ += length;
		return true;
	}

	bool AtEnd() const { return offset_ == length_; }

private:
	const char *data_;
	size_t length_;
	size_t offset_;
};

void Serialize(const ParseResult &result, std::string *out)
{
	out->clear();
	AppendUInt32(out, kBlobMagic);
	AppendUInt32(out, result.parsed ? 1 : 0);
	AppendString(out, result.failure);
	AppendString(out, result.summary);
	AppendString(out, result.mainModuleCodeFile);
	AppendUInt32(out, static_cast<uint32_t>(result.modules.size()));
	for (size_t index = 0; index < result.modules.size(); ++index) {
		const ModuleInfo &module = result.modules[index];
		AppendString(out, module.codeFile);
		AppendString(out, module.debugFile);
		AppendString(out, module.debugIdentifier);
		AppendString(out, module.codeIdentifier);
	}
	AppendUInt32(out, kBlobEndMagic);
}

bool Deserialize(const std::string &blob, ParseResult *result)
{
	BlobReader reader(blob.data(), blob.size());

	uint32_t magic = 0;
	uint32_t parsed = 0;
	uint32_t moduleCount = 0;
	if (!reader.UInt32(&magic) || magic != kBlobMagic || !reader.UInt32(&parsed)) {
		return false;
	}
	if (!reader.String(&result->failure)
		|| !reader.String(&result->summary)
		|| !reader.String(&result->mainModuleCodeFile)) {
		return false;
	}
	if (!reader.UInt32(&moduleCount)) {
		return false;
	}

	result->modules.clear();
	for (uint32_t index = 0; index < moduleCount; ++index) {
		ModuleInfo module;
		if (!reader.String(&module.codeFile)
			|| !reader.String(&module.debugFile)
			|| !reader.String(&module.debugIdentifier)
			|| !reader.String(&module.codeIdentifier)) {
			return false;
		}
		result->modules.push_back(module);
	}

	uint32_t endMagic = 0;
	if (!reader.UInt32(&endMagic) || endMagic != kBlobEndMagic || !reader.AtEnd()) {
		return false;
	}

	result->parsed = (parsed != 0);
	return true;
}

/**
 * @brief Body of the forked child: parse, write the result, leave.
 *
 * The child inherits the server's threads and locks, so it touches nothing but
 * the dump and its own copy of this file's state, and exits through _exit().
 */
void ParseChild(const char *path, int resultFd)
{
	// The child inherits the server's crash handlers, breakpad's included.
	// Without this a fault here would make breakpad write yet another dump into
	// the folder this run is working through, which is the situation being
	// fixed; the parent reports the child's death instead.
	static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
	for (size_t index = 0; index < sizeof(signals) / sizeof(signals[0]); ++index) {
		signal(signals[index], SIG_DFL);
	}

	ParseResult result;
	ParseInPlace(path, &result);

	std::string blob;
	Serialize(result, &blob);

	size_t offset = 0;
	while (offset < blob.size()) {
		ssize_t written = write(resultFd, blob.data() + offset, blob.size() - offset);
		if (written <= 0) {
			if (written < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		offset += static_cast<size_t>(written);
	}

	close(resultFd);
	_exit(0);
}

/**
 * @brief Parses the dump in a child process and reads the result back.
 *
 * @return False when no child could be started, so the caller can fall back.
 */
bool ParseInChild(const char *path, ParseResult *result)
{
	int fds[2];
	if (pipe(fds) != 0) {
		return false;
	}

	pid_t child = fork();
	if (child < 0) {
		close(fds[0]);
		close(fds[1]);
		return false;
	}

	if (child == 0) {
		close(fds[0]);

		// The parser's output is not wanted on the server's console. dup2 rather
		// than freopen: freopen flushes whatever the parent had buffered, which
		// would print a half-written line a second time.
		int devNull = open("/dev/null", O_WRONLY);
		if (devNull >= 0) {
			dup2(devNull, STDOUT_FILENO);
			dup2(devNull, STDERR_FILENO);
			if (devNull > STDERR_FILENO) {
				close(devNull);
			}
		}

		ParseChild(path, fds[1]);
		_exit(0);
	}

	close(fds[1]);

	// A deadline, not a per-wait timeout: a child that trickles bytes would
	// otherwise keep the upload thread busy for as long as it liked.
	time_t deadline = time(nullptr) + kParseTimeoutSeconds;

	std::string blob;
	bool wroteEverything = true;
	bool childClosed = false;
	while (!childClosed) {
		long remainingMs = static_cast<long>(deadline - time(nullptr)) * 1000;
		if (remainingMs <= 0) {
			wroteEverything = false;
			break;
		}

		struct pollfd pollFd;
		pollFd.fd = fds[0];
		pollFd.events = POLLIN;
		pollFd.revents = 0;

		int ready = poll(&pollFd, 1, static_cast<int>(remainingMs));
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			wroteEverything = false;
			break;
		}
		if (ready == 0) {
			wroteEverything = false;
			break;
		}

		char chunk[4096];
		ssize_t got = read(fds[0], chunk, sizeof(chunk));
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			wroteEverything = false;
			break;
		}
		if (got == 0) {
			childClosed = true;
			break;
		}

		blob.append(chunk, static_cast<size_t>(got));
		if (blob.size() > kMaxBlobSize) {
			wroteEverything = false;
			break;
		}
	}

	close(fds[0]);

	if (!wroteEverything || !childClosed) {
		// Nothing more can be useful from this child, and the upload thread
		// cannot sit on a wedged process.
		kill(child, SIGKILL);
	}

	int status = 0;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR) {
			break;
		}
	}

	if (!wroteEverything || !childClosed) {
		char reason[160];
		if (WIFSIGNALED(status)) {
			snprintf(reason, sizeof(reason),
				"the minidump parser had to be killed on signal %d", WTERMSIG(status));
		} else {
			snprintf(reason, sizeof(reason),
				"the minidump parser stopped responding after %d seconds",
				kParseTimeoutSeconds);
		}
		result->failure = reason;
		return true;
	}

	if (WIFSIGNALED(status)) {
		// The case the server used to die of: breakpad's parser faulted.
		char reason[160];
		snprintf(reason, sizeof(reason),
			"the minidump parser crashed on signal %d while reading this dump",
			WTERMSIG(status));
		result->failure = reason;
		return true;
	}

	if (!Deserialize(blob, result)) {
		result->failure = "the minidump parser returned an unreadable result";
		return true;
	}

	return true;
}

#endif // _LINUX

} // namespace

bool Validate(const char *path, char *reason, size_t reasonLength)
{
	if (reason && reasonLength) {
		reason[0] = '\0';
	}

	FILE *file = fopen(path, "rb");
	if (!file) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength, "could not open the dump");
		}
		return false;
	}

	bool valid = false;
	unsigned char header[kMinidumpHeaderSize];
	unsigned char entry[kMinidumpDirectorySize];

	size_t headerRead = fread(header, 1, sizeof(header), file);
	if (headerRead != sizeof(header)) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength, "the file is too short to be a minidump (%u bytes)",
				static_cast<unsigned>(headerRead));
		}
		fclose(file);
		return false;
	}

	if (memcmp(header, kMinidumpMagic, sizeof(kMinidumpMagic)) != 0) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength, "the file does not start with the minidump signature");
		}
		fclose(file);
		return false;
	}

	long fileSize = -1;
	if (fseek(file, 0, SEEK_END) != 0 || (fileSize = ftell(file)) < 0) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength, "the dump's size could not be determined");
		}
		fclose(file);
		return false;
	}

	uint32_t streamCount = ReadUInt32LE(&header[8]);
	uint32_t directoryRva = ReadUInt32LE(&header[12]);

	if (streamCount == 0) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength, "the dump has no streams");
		}
		fclose(file);
		return false;
	}

	// The directory has to be inside the file, and every stream it names has to
	// fit: an out of range RVA is how a dump written by a half-dead process gets
	// breakpad to read outside the mapping.
	uint64_t directoryBytes = static_cast<uint64_t>(streamCount) * kMinidumpDirectorySize;
	if (static_cast<uint64_t>(directoryRva) < kMinidumpHeaderSize
		|| static_cast<uint64_t>(directoryRva) + directoryBytes
			> static_cast<uint64_t>(fileSize)) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength,
				"the stream directory is outside the file (%u streams at 0x%x, file is %ld bytes)",
				streamCount, directoryRva, fileSize);
		}
		fclose(file);
		return false;
	}

	if (fseek(file, static_cast<long>(directoryRva), SEEK_SET) != 0) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength, "the stream directory could not be read");
		}
		fclose(file);
		return false;
	}

	bool sawThreadList = false;
	bool sawModuleList = false;
	for (uint32_t index = 0; index < streamCount; ++index) {
		if (fread(entry, 1, sizeof(entry), file) != sizeof(entry)) {
			if (reason && reasonLength) {
				snprintf(reason, reasonLength, "the stream directory is truncated");
			}
			fclose(file);
			return false;
		}

		uint32_t streamType = ReadUInt32LE(&entry[0]);
		uint32_t streamRva = ReadUInt32LE(&entry[4]);
		uint32_t streamSize = ReadUInt32LE(&entry[8]);

		if (streamType == 0) {
			// MD_UNUSED_STREAM: a hole in the directory, not a payload.
			continue;
		}

		if (static_cast<uint64_t>(streamRva) + static_cast<uint64_t>(streamSize)
			> static_cast<uint64_t>(fileSize)) {
			if (reason && reasonLength) {
				snprintf(reason, reasonLength,
					"stream %u (type %u) points outside the file", index, streamType);
			}
			fclose(file);
			return false;
		}

		if (streamType == kStreamThreadList) {
			sawThreadList = true;

			// MDRawThreadList starts with its element count, which is the first
			// thing breakpad walks. A count no server could have means the dump
			// is damaged, and it is refused here rather than in the parser.
			unsigned char threadCountBytes[4];
			long returnOffset = ftell(file);
			if (streamSize >= sizeof(threadCountBytes)
				&& fseek(file, static_cast<long>(streamRva), SEEK_SET) == 0
				&& fread(threadCountBytes, 1, sizeof(threadCountBytes), file) == sizeof(threadCountBytes)) {
				uint32_t threadCount = ReadUInt32LE(threadCountBytes);
				if (threadCount > kMaxPlausibleThreads) {
					if (reason && reasonLength) {
						snprintf(reason, reasonLength,
							"the dump claims %u threads, which cannot be right", threadCount);
					}
					fclose(file);
					return false;
				}
			}

			if (returnOffset >= 0 && fseek(file, returnOffset, SEEK_SET) != 0) {
				if (reason && reasonLength) {
					snprintf(reason, reasonLength, "the stream directory could not be read");
				}
				fclose(file);
				return false;
			}
		} else if (streamType == kStreamModuleList) {
			sawModuleList = true;
		}
	}

	// Without these a signature cannot be built at all, and the processor would
	// fail on them after reading the whole file.
	if (!sawThreadList || !sawModuleList) {
		if (reason && reasonLength) {
			snprintf(reason, reasonLength, "the dump is missing its %s stream",
				sawThreadList ? "module list" : "thread list");
		}
		fclose(file);
		return false;
	}

	valid = true;
	fclose(file);
	return valid;
}

bool ParseRunsInOwnProcess()
{
#if defined _LINUX
	return true;
#else
	return false;
#endif
}

bool Parse(const char *path, ParseResult *result)
{
	*result = ParseResult();

#if defined _LINUX
	// A pipe, then a child: the child owns the parse and any fault in it, and the
	// parent only ever reads a bounded result (or an empty pipe).
	if (ParseInChild(path, result)) {
		return result->parsed;
	}
	// No pipe or no child (resource exhaustion); a parse in this process is
	// still better than dropping the dump, and Validate() has already vetted it.
#endif

	return ParseInPlace(path, result);
}

}