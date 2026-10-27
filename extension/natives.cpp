#include <algorithm>
#include <amtl/am-string.h>
#include "extension.h"
#include "natives.h"
#include "sp_stack.h"

static cell_t Native_GetUploadedCrashCount(IPluginContext* context, const cell_t* params)
{
	return g_accelerator.GetUploadedCrashCount();
}

static cell_t Native_IsDoneUploadingCrashes(IPluginContext* context, const cell_t* params)
{
	if (g_accelerator.IsDoneUploading()) {
		return 1;
	}

	return 0;
}

static cell_t Native_GetCrashHTTPResponse(IPluginContext* context, const cell_t* params)
{
	if (!g_accelerator.IsDoneUploading()) {
		context->ReportError("Wait until accelerator is done uploading crashes before accessing crash information!");
		return 0;
	}

	int element = static_cast<int>(params[1]);
	const UploadedCrash* crash = g_accelerator.GetUploadedCrash(element);

	if (!crash) {
		context->ReportError("Crash index %i is invalid!", element);
		return 0;
	}

	char* buffer;
	context->LocalToString(params[2], &buffer);

	size_t maxsize = static_cast<size_t>(params[3]);

	ke::SafeStrcpy(buffer, maxsize, crash->GetHTTPResponse().c_str());

	return 0;
}

/**
 * @brief Writes the live SourcePawn call stack into a plugin string.
 *
 * This is the same walk the crash handler runs, so a plugin can prove on a
 * running server that the VM's frame iterator is usable with this SourcePawn
 * build, without waiting for a crash.
 */
static cell_t Native_GetPluginStack(IPluginContext* context, const cell_t* params)
{
	char* buffer;
	context->LocalToString(params[1], &buffer);

	size_t size = static_cast<size_t>(params[2]);
	if (params[2] <= 0 || size == 0) {
		buffer[0] = '\0';
		return 0;
	}

	// The iterator is created and destroyed inside this call; leaving one alive
	// would forbid the VM from being re-entered.
	spstack::Output out(buffer, size);
	return spstack::WriteStackFrames(out, 64, context);
}

void natives::Setup(std::vector<sp_nativeinfo_t>& vec)
{
	sp_nativeinfo_t list[] = {
		{"Accelerator_GetUploadedCrashCount", Native_GetUploadedCrashCount},
		{"Accelerator_IsDoneUploadingCrashes", Native_IsDoneUploadingCrashes},
		{"Accelerator_GetCrashHTTPResponse", Native_GetCrashHTTPResponse},
		{"Accelerator_GetPluginStack", Native_GetPluginStack},
	};

	vec.insert(vec.end(), std::begin(list), std::end(list));
}
