// SC4MCP - MCP tools, resources and prompts.
#pragma once
#include "json.hpp"

#include <functional>
#include <string>

namespace Tools
{
	using json = nlohmann::json;

	// Progress of a long tool: progress out of total (total 0 = unknown), with a short message.
	using Progress = std::function<void(double progress, double total, const std::string& message)>;

	// Asks the user to confirm a change through the client (MCP elicitation). Returns 1 = accepted, 0 = declined or
	// cancelled, -1 = the client cannot ask.
	using Confirm = std::function<int(const std::string& message)>;

	// What the protocol layer offers a running tool.
	struct Context
	{
		Progress progress;
		Confirm confirm;
	};

	// Tool definitions for tools/list.
	json List();

	// Runs a tool. Returns the result object; sets isError when the tool failed.
	json Call(const std::string& name, const json& arguments, bool& isError, const Context& context = {});

	// Resources (resources/list, resources/read): read-only views of the city.
	json ListResources();
	// Returns the resource contents, or null when the URI is unknown.
	json ReadResource(const std::string& uri);

	// Prompts (prompts/list, prompts/get): ready-made task descriptions.
	json ListPrompts();
	// Returns { description, messages }, or null when the prompt is unknown.
	json GetPrompt(const std::string& name, const json& arguments);

	// Settings read from SC4MCP.ini at startup. confirmChanges: tools that change the city ask for a confirmation
	// (elicitation, or confirm=true) before applying.
	void Configure(bool allowControl, bool allowCheats, bool allowBuild, bool confirmChanges);
}
