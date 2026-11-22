// SC4MCP - log file (SC4MCP.log next to the DLL).
#include "McpLog.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
	std::mutex logMutex;
	FILE* logFile = nullptr;

	HMODULE ThisModule()
	{
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ThisModule), &module);
		return module;
	}
}

std::filesystem::path McpLog::DllFolder()
{
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(ThisModule(), path, MAX_PATH);
	return std::filesystem::path(path).parent_path();
}

void McpLog::Open()
{
	std::lock_guard<std::mutex> lock(logMutex);
	if (logFile) return;
	const std::filesystem::path file = DllFolder() / L"SC4MCP.log";
	logFile = _wfopen(file.c_str(), L"w");
}

void McpLog::Write(const char* format, ...)
{
	std::lock_guard<std::mutex> lock(logMutex);
	if (!logFile) return;
	SYSTEMTIME now;
	GetLocalTime(&now);
	std::fprintf(logFile, "%02u:%02u:%02u ", now.wHour, now.wMinute, now.wSecond);
	va_list args;
	va_start(args, format);
	std::vfprintf(logFile, format, args);
	va_end(args);
	std::fputc('\n', logFile);
	std::fflush(logFile);
}
