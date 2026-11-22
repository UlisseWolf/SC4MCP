// SC4MCP - MCP tools: definitions (name, description, input schema) and implementations.
#pragma once
#include "json.hpp"

namespace Tools
{
	using json = nlohmann::json;

	// Tool definitions for tools/list.
	json List();

	// Runs a tool. Returns the result object; sets isError when the tool failed.
	json Call(const std::string& name, const json& arguments, bool& isError);

	// Settings read from SC4MCP.ini at startup.
	void Configure(bool allowControl, bool allowCheats);
}
