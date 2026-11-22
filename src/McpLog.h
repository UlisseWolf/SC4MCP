// SC4MCP - log file (SC4MCP.log next to the DLL).
#pragma once
#include <filesystem>
#include <string>

namespace McpLog
{
	void Open();
	void Write(const char* format, ...);
	// Folder of SC4MCP.dll (the Plugins folder).
	std::filesystem::path DllFolder();
}
