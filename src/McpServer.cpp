// SC4MCP - MCP JSON-RPC protocol.
//
// Implements initialize, ping, tools/list, tools/call, resources/list, resources/read, prompts/list and prompts/get
// over the Streamable HTTP transport. Responses are a single JSON body, except when a tools/call from a client that
// accepts text/event-stream sends progress notifications (it gave a progressToken) or asks the user to confirm a
// change (elicitation, when the client declared it): then the notifications, the elicitation/create request and the
// result are server-sent events on that response, and the client POSTs its answer to the elicitation as a separate
// request. GET /mcp answers 405 because the server never pushes messages on its own. With AuthToken set in
// SC4MCP.ini, /mcp requires "Authorization: Bearer <token>". GET /setup shows the client configurations.
#include "McpServer.h"
#include "McpLog.h"
#include "Tools.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

using nlohmann::json;

namespace
{
	constexpr const char* kServerName = "SC4MCP";
	constexpr const char* kServerVersion = "1.2.0";
	std::string authToken;
	uint16_t serverPort = 8765;
	std::atomic<bool> clientElicitation{ false };

	// Elicitations waiting for the client's answer, by request id.
	struct PendingAnswer { bool done = false; json response; };
	std::mutex pendingMutex;
	std::condition_variable pendingAnswered;
	std::map<std::string, PendingAnswer> pendingAnswers;
	std::atomic<unsigned> elicitationCounter{ 0 };

	std::string IdKey(const json& id) { return id.is_string() ? id.get<std::string>() : id.dump(); }

	// A JSON-RPC response from the client (no method, an id and a result or an error).
	bool IsClientResponse(const json& message)
	{
		return message.is_object() && !message.contains("method") && message.contains("id") && (message.contains("result") || message.contains("error"));
	}

	bool DeliverClientResponse(const json& message)
	{
		std::lock_guard<std::mutex> lock(pendingMutex);
		const auto found = pendingAnswers.find(IdKey(message["id"]));
		if (found == pendingAnswers.end()) return false;
		found->second.done = true;
		found->second.response = message;
		pendingAnswered.notify_all();
		return true;
	}

	// Sends elicitation/create on the stream and waits (at most 3 minutes) for the user's answer.
	int AskUser(HttpServer::Stream& stream, const std::string& message)
	{
		const std::string id = "sc4mcp-confirm-" + std::to_string(++elicitationCounter);
		{
			std::lock_guard<std::mutex> lock(pendingMutex);
			pendingAnswers[id] = {};
		}
		const json request{ {"jsonrpc", "2.0"}, {"id", id}, {"method", "elicitation/create"}, {"params", {
			{"message", message},
			{"requestedSchema", {{"type", "object"}, {"properties", {{"confirm", {{"type", "boolean"}, {"title", "Apply this change"},
				{"description", "Leave on to apply the change in the city."}, {"default", true}}}}}, {"required", json::array()}}} }} };
		stream.Event(request.dump());
		McpLog::Write("Asking the user to confirm: %s", message.c_str());
		std::unique_lock<std::mutex> lock(pendingMutex);
		const bool answered = pendingAnswered.wait_for(lock, std::chrono::minutes(3), [&] { return pendingAnswers[id].done; });
		const json response = pendingAnswers[id].response;
		pendingAnswers.erase(id);
		lock.unlock();
		if (!answered || !response.contains("result")) return 0;
		const json& result = response["result"];
		const bool accepted = result.value("action", std::string()) == "accept"
			&& !(result.contains("content") && result["content"].is_object() && result["content"].value("confirm", true) == false);
		McpLog::Write("The user %s the change.", accepted ? "accepted" : "declined");
		return accepted ? 1 : 0;
	}
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

	// Returns a response, or null for a notification. stream (may be null) carries progress and elicitations.
	json HandleMessage(const json& message, const Notify& notify = {}, HttpServer::Stream* stream = nullptr)
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
			clientElicitation = params.contains("capabilities") && params["capabilities"].is_object() && params["capabilities"].contains("elicitation");
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
			Tools::Confirm confirm;
			if (stream && clientElicitation) confirm = [stream](const std::string& text) { return AskUser(*stream, text); };
			json output = Tools::Call(name, arguments, isError, Tools::Context{ progress, confirm });
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

void McpServer::Configure(const std::string& token, uint16_t port)
{
	authToken = token;
	serverPort = port;
}

namespace
{
	std::string SetupPage()
	{
		const std::string url = "http://127.0.0.1:" + std::to_string(serverPort) + "/mcp";
		const bool hasToken = !authToken.empty();
		const std::string bearer = "Bearer " + authToken;
		auto block = [](const std::string& title, const std::string& where, const std::string& code)
		{
			std::string escaped;
			for (char c : code) { if (c == '<') escaped += "&lt;"; else if (c == '>') escaped += "&gt;"; else if (c == '&') escaped += "&amp;"; else escaped += c; }
			return "<section><h2>" + title + "</h2><p>" + where + "</p><pre><code>" + escaped + "</code></pre></section>\n";
		};
		const std::string headersJson = hasToken ? ",\n      \"headers\": { \"Authorization\": \"" + bearer + "\" }" : "";
		std::string html = "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>SC4MCP setup</title><style>"
			"body{font-family:Segoe UI,Arial,sans-serif;max-width:860px;margin:2em auto;padding:0 1em;color:#222}"
			"pre{background:#f4f4f4;padding:1em;overflow-x:auto;border-radius:6px}h1{margin-bottom:.2em}h2{margin-top:1.6em}"
			".note{background:#fff8e1;padding:.8em 1em;border-radius:6px}</style></head><body>"
			"<h1>SC4MCP " + std::string(kServerVersion) + "</h1><p>The MCP server of this game is at <code>" + url + "</code>. "
			"Copy the configuration of your AI client, save it and restart the client.</p>";
		if (hasToken) html += "<p class=\"note\">This server requires a token: it is already filled in below. Keep it private.</p>";
		html += block("Claude Code", "Run in a terminal:", "claude mcp add --transport http sc4 " + url + (hasToken ? " --header \"Authorization: " + bearer + "\"" : ""));
		html += block("Claude Desktop", "Settings &gt; Developer &gt; Edit Config (needs Node.js), then quit Claude Desktop completely and start it again:",
			"{\n  \"mcpServers\": {\n    \"sc4\": {\n      \"command\": \"npx\",\n      \"args\": [\"-y\", \"mcp-remote\", \"" + url + "\""
			+ (hasToken ? ", \"--header\", \"Authorization: " + bearer + "\"" : "") + "]\n    }\n  }\n}");
		html += block("Cursor", "%USERPROFILE%\\.cursor\\mcp.json:",
			"{\n  \"mcpServers\": {\n    \"sc4\": {\n      \"url\": \"" + url + "\"" + headersJson + "\n    }\n  }\n}");
		html += block("VS Code (GitHub Copilot agent mode)", ".vscode/mcp.json, or MCP: Open User Configuration:",
			"{\n  \"servers\": {\n    \"sc4\": {\n      \"type\": \"http\",\n      \"url\": \"" + url + "\"" + headersJson + "\n    }\n  }\n}");
		html += block("Windsurf", "%USERPROFILE%\\.codeium\\windsurf\\mcp_config.json:",
			"{\n  \"mcpServers\": {\n    \"sc4\": {\n      \"serverUrl\": \"" + url + "\"" + headersJson + "\n    }\n  }\n}");
		html += block("Gemini CLI", "%USERPROFILE%\\.gemini\\settings.json:",
			"{\n  \"mcpServers\": {\n    \"sc4\": {\n      \"httpUrl\": \"" + url + "\"" + headersJson + "\n    }\n  }\n}");
		html += block("Codex CLI and ChatGPT desktop app", "%USERPROFILE%\\.codex\\config.toml:",
			"[mcp_servers.sc4]\nurl = \"" + url + "\"\ntool_timeout_sec = 300" + (hasToken ? "\nhttp_headers = { \"Authorization\" = \"" + bearer + "\" }" : ""));
		html += block("LM Studio", "Program &gt; Install &gt; Edit mcp.json:",
			"{\n  \"mcpServers\": {\n    \"sc4\": {\n      \"url\": \"" + url + "\"" + headersJson + "\n    }\n  }\n}");
		html += "<p>ChatGPT on the web and claude.ai cannot reach this PC: use one of the clients above. "
			"More in the README: https://github.com/UlisseWolf/SC4MCP</p></body></html>";
		return html;
	}
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
	if (path == "/setup" && method == "GET") return { 200, "text/html; charset=utf-8", SetupPage() };
	if (path != "/mcp") return { 404, "text/plain", "Not found. The MCP endpoint is /mcp; the client configurations are at /setup" };
	if (method != "POST") return { 405, "text/plain", "Use POST" };
	if (!Authorized(http.authorization))
	{
		McpLog::Write("Request refused: missing or wrong Authorization token.");
		return { 401, "application/json", Error(nullptr, -32001, "Unauthorized: set the AuthToken of SC4MCP.ini as a Bearer token.").dump() };
	}

	json request = json::parse(body, nullptr, false);
	if (request.is_discarded()) return { 400, "application/json", Error(nullptr, -32700, "Parse error").dump() };

	// Answers of the client to an elicitation (possibly in a batch).
	if (IsClientResponse(request))
	{
		DeliverClientResponse(request);
		return { 202, "application/json", "" };
	}
	if (request.is_array() && !request.empty() && std::all_of(request.begin(), request.end(), IsClientResponse))
	{
		for (const json& message : request) DeliverClientResponse(message);
		return { 202, "application/json", "" };
	}

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
	// A single tools/call from a client that accepts SSE: progress notifications and elicitations go on the stream,
	// which starts only when the tool sends one; otherwise the answer is a plain JSON body.
	const bool canStream = request.is_object() && request.value("method", "") == "tools/call" && http.accept.find("text/event-stream") != std::string::npos;
	if (canStream)
	{
		Notify notify = [&stream](const json& notification) { stream.Event(notification.dump()); };
		json response = HandleMessage(request, notify, &stream);
		if (stream.Started())
		{
			stream.Event(response.dump());
			HttpServer::Response streamed;
			streamed.streamed = true;
			return streamed;
		}
		return { 200, "application/json", response.dump() };
	}

	json response = HandleMessage(request);
	if (response.is_null()) return { 202, "application/json", "" };
	return { 200, "application/json", response.dump() };
}
