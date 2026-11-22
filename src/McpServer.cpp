// SC4MCP - MCP JSON-RPC protocol.
//
// Implements the parts of the Model Context Protocol a tool server needs: initialize, ping, tools/list and
// tools/call, over the Streamable HTTP transport. Every response is a single JSON body (no SSE stream), which the
// specification allows; GET /mcp answers 405 because the server never pushes messages.
#include "McpServer.h"
#include "McpLog.h"
#include "Tools.h"
#include "json.hpp"

using nlohmann::json;

namespace
{
	constexpr const char* kServerName = "SC4MCP";
	constexpr const char* kServerVersion = "1.0.0";
	const char* const kProtocols[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

	constexpr const char* kInstructions =
		"SC4MCP reads and controls the SimCity 4 city that is open in the game. "
		"Start with get_city_status. Money values are in simoleons; monthly values are the game's current estimates. "
		"Use wait_months to let time pass, then compare get_budget before and after. "
		"The game must be running with a city open; while a dialog is open the tools time out.";

	json Error(const json& id, int code, const std::string& message)
	{
		return json{ {"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}} };
	}

	json Result(const json& id, json result)
	{
		return json{ {"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)} };
	}

	// Returns a response, or null for a notification.
	json HandleMessage(const json& message)
	{
		if (!message.is_object() || message.value("jsonrpc", "") != "2.0" || !message.contains("method") || !message["method"].is_string())
		{
			return Error(message.is_object() && message.contains("id") ? message["id"] : json(nullptr), -32600, "Invalid Request");
		}
		const std::string method = message["method"].get<std::string>();
		const bool isNotification = !message.contains("id");
		const json id = isNotification ? json(nullptr) : message["id"];
		const json params = message.contains("params") ? message["params"] : json::object();

		if (isNotification)
		{
			if (method == "notifications/initialized") McpLog::Write("Client initialized.");
			return nullptr;
		}

		if (method == "initialize")
		{
			const std::string requested = params.value("protocolVersion", std::string());
			std::string version = kProtocols[0];
			for (const char* supported : kProtocols)
			{
				if (requested == supported) version = supported;
			}
			std::string client = "unknown";
			if (params.contains("clientInfo") && params["clientInfo"].is_object()) client = params["clientInfo"].value("name", client);
			McpLog::Write("initialize from %s (protocol %s -> %s)", client.c_str(), requested.c_str(), version.c_str());
			return Result(id, json{ {"protocolVersion", version},
				{"capabilities", {{"tools", {{"listChanged", false}}}}},
				{"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
				{"instructions", kInstructions} });
		}
		if (method == "ping") return Result(id, json::object());
		if (method == "tools/list") return Result(id, json{ {"tools", Tools::List()} });
		if (method == "tools/call")
		{
			if (!params.contains("name") || !params["name"].is_string()) return Error(id, -32602, "Missing tool name");
			const std::string name = params["name"].get<std::string>();
			const json arguments = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
			bool isError = false;
			json output = Tools::Call(name, arguments, isError);
			McpLog::Write("tools/call %s%s", name.c_str(), isError ? " (error)" : "");
			json result{ {"content", json::array({ {{"type", "text"}, {"text", output.dump(2)}} })}, {"isError", isError} };
			if (output.is_object()) result["structuredContent"] = output;
			return Result(id, result);
		}
		return Error(id, -32601, "Method not found: " + method);
	}
}

HttpServer::Response McpServer::HandleHttp(const std::string& method, const std::string& path, const std::string& body)
{
	if (path == "/health" && method == "GET")
	{
		return { 200, "application/json", json{ {"server", kServerName}, {"version", kServerVersion}, {"ok", true} }.dump() };
	}
	if (path != "/mcp") return { 404, "text/plain", "Not found. The MCP endpoint is /mcp" };
	if (method != "POST") return { 405, "text/plain", "Use POST" };

	json request = json::parse(body, nullptr, false);
	if (request.is_discarded()) return { 400, "application/json", Error(nullptr, -32700, "Parse error").dump() };

	if (request.is_array())
	{
		json responses = json::array();
		for (const json& message : request)
		{
			json response = HandleMessage(message);
			if (!response.is_null()) responses.push_back(std::move(response));
		}
		if (responses.empty()) return { 202, "application/json", "" };
		return { 200, "application/json", responses.dump() };
	}
	json response = HandleMessage(request);
	if (response.is_null()) return { 202, "application/json", "" };
	return { 200, "application/json", response.dump() };
}
