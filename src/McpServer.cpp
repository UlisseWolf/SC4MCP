// SC4MCP - MCP JSON-RPC protocol.
//
// Implements initialize, ping, tools/list, tools/call, resources/list, resources/read, prompts/list and prompts/get
// over the Streamable HTTP transport. Responses are a single JSON body, except a tools/call that carries a
// progressToken from a client that accepts text/event-stream: its progress notifications and its result are sent as
// server-sent events on the same response. GET /mcp answers 405 because the server never pushes messages on its own.
// With AuthToken set in SC4MCP.ini, /mcp requires "Authorization: Bearer <token>".
#include "McpServer.h"
#include "McpLog.h"
#include "Tools.h"
#include "json.hpp"

#include <functional>

using nlohmann::json;

namespace
{
	constexpr const char* kServerName = "SC4MCP";
	constexpr const char* kServerVersion = "1.1.0";
	std::string authToken;
	const char* const kProtocols[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

	constexpr const char* kInstructions =
		"SC4MCP reads and controls the SimCity 4 city that is open in the game. "
		"Start with get_city_status. Money values are in simoleons; monthly values are the game's current estimates. "
		"Use wait_months to let time pass, then compare get_budget, get_services and get_utilities before and after; "
		"get_history and list_graphs give the monthly history the Graphs window plots. Cells are (x, z) city cells, as "
		"list_buildings and query_cell report them. Tools that change the city (funding, taxes, ordinances, zoning, "
		"saving) should only be used when the user asks. "
		"The game must be running with a city open; while a dialog is open the tools time out.";

	json Error(const json& id, int code, const std::string& message)
	{
		return json{ {"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}} };
	}

	json Result(const json& id, json result)
	{
		return json{ {"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)} };
	}

	// Sends a progress notification for a tools/call that asked for it.
	using Notify = std::function<void(const json& notification)>;

	// Returns a response, or null for a notification.
	json HandleMessage(const json& message, const Notify& notify = {})
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
				{"capabilities", {{"tools", {{"listChanged", false}}}, {"resources", {{"listChanged", false}, {"subscribe", false}}}, {"prompts", {{"listChanged", false}}}}},
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
			Tools::Progress progress;
			if (notify && params.contains("_meta") && params["_meta"].is_object() && params["_meta"].contains("progressToken"))
			{
				const json token = params["_meta"]["progressToken"];
				progress = [token, notify](double done, double total, const std::string& text)
				{
					json p{ {"progressToken", token}, {"progress", done}, {"message", text} };
					if (total > 0) p["total"] = total;
					notify(json{ {"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p} });
				};
			}
			json output = Tools::Call(name, arguments, isError, progress);
			McpLog::Write("tools/call %s%s", name.c_str(), isError ? " (error)" : "");
			json result{ {"content", json::array({ {{"type", "text"}, {"text", output.dump(2)}} })}, {"isError", isError} };
			if (output.is_object()) result["structuredContent"] = output;
			return Result(id, result);
		}
		if (method == "resources/list") return Result(id, json{ {"resources", Tools::ListResources()} });
		if (method == "resources/templates/list") return Result(id, json{ {"resourceTemplates", json::array()} });
		if (method == "resources/read")
		{
			const std::string uri = params.value("uri", std::string());
			json contents = Tools::ReadResource(uri);
			if (contents.is_null()) return Error(id, -32002, "Resource not found: " + uri);
			return Result(id, json{ {"contents", contents} });
		}
		if (method == "prompts/list") return Result(id, json{ {"prompts", Tools::ListPrompts()} });
		if (method == "prompts/get")
		{
			const std::string name = params.value("name", std::string());
			json prompt = Tools::GetPrompt(name, params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object());
			if (prompt.is_null()) return Error(id, -32602, "Unknown prompt: " + name);
			return Result(id, prompt);
		}
		return Error(id, -32601, "Method not found: " + method);
	}

	// Constant-time comparison of the bearer token.
	bool Authorized(const std::string& header)
	{
		if (authToken.empty()) return true;
		const std::string prefix = "Bearer ";
		if (header.size() < prefix.size() || header.compare(0, prefix.size(), prefix) != 0) return false;
		const std::string given = header.substr(prefix.size());
		unsigned char diff = static_cast<unsigned char>(given.size() != authToken.size());
		for (size_t i = 0; i < authToken.size(); ++i) diff |= static_cast<unsigned char>(authToken[i] ^ (i < given.size() ? given[i] : 0));
		return diff == 0;
	}
}

void McpServer::Configure(const std::string& token)
{
	authToken = token;
}

HttpServer::Response McpServer::HandleHttp(const HttpServer::Request& http, HttpServer::Stream& stream)
{
	const std::string& method = http.method;
	const std::string& path = http.path;
	const std::string& body = http.body;
	if (path == "/health" && method == "GET")
	{
		return { 200, "application/json", json{ {"server", kServerName}, {"version", kServerVersion}, {"ok", true} }.dump() };
	}
	if (path != "/mcp") return { 404, "text/plain", "Not found. The MCP endpoint is /mcp" };
	if (method != "POST") return { 405, "text/plain", "Use POST" };
	if (!Authorized(http.authorization))
	{
		McpLog::Write("Request refused: missing or wrong Authorization token.");
		return { 401, "application/json", Error(nullptr, -32001, "Unauthorized: set the AuthToken of SC4MCP.ini as a Bearer token.").dump() };
	}

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
	// A single tools/call with a progress token, from a client that accepts SSE: stream the progress and the result.
	const bool wantsProgress = request.is_object() && request.value("method", "") == "tools/call" && request.contains("params")
		&& request["params"].is_object() && request["params"].contains("_meta") && request["params"]["_meta"].is_object()
		&& request["params"]["_meta"].contains("progressToken") && http.accept.find("text/event-stream") != std::string::npos;
	if (wantsProgress)
	{
		Notify notify = [&stream](const json& notification) { stream.Event(notification.dump()); };
		json response = HandleMessage(request, notify);
		stream.Event(response.dump());
		HttpServer::Response streamed;
		streamed.streamed = true;
		return streamed;
	}

	json response = HandleMessage(request);
	if (response.is_null()) return { 202, "application/json", "" };
	return { 200, "application/json", response.dump() };
}
