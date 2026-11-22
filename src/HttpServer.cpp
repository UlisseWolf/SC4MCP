// SC4MCP - minimal HTTP/1.1 server (Winsock2), bound to 127.0.0.1 only.
//
// One request per connection ("Connection: close"), handled on a single background thread. This is enough for an
// MCP client on the same machine and keeps the game thread untouched: tools reach the game through MainThreadQueue.
// A handler may answer with server-sent events (progress notifications of long tools) through HttpServer::Stream.
#include "HttpServer.h"
#include "McpLog.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <thread>

namespace
{
	constexpr size_t kMaxHeader = 16 * 1024;
	constexpr size_t kMaxBody = 1024 * 1024;

	SOCKET listener = INVALID_SOCKET;
	std::thread worker;
	std::atomic<bool> running{ false };
	HttpServer::Handler handler;

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	std::string Trim(const std::string& text)
	{
		const size_t first = text.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return {};
		const size_t last = text.find_last_not_of(" \t\r\n");
		return text.substr(first, last - first + 1);
	}

	std::string HeaderValue(const std::string& headers, const std::string& name)
	{
		const std::string lower = Lower(headers);
		const std::string key = "\r\n" + Lower(name) + ":";
		const size_t at = lower.find(key);
		if (at == std::string::npos) return {};
		const size_t start = at + key.size();
		const size_t end = headers.find("\r\n", start);
		return Trim(headers.substr(start, end == std::string::npos ? std::string::npos : end - start));
	}

	// DNS-rebinding protection: browsers send Origin; only local origins may call the server.
	bool OriginAllowed(const std::string& origin)
	{
		if (origin.empty() || origin == "null") return true;
		const std::string o = Lower(origin);
		for (const char* prefix : { "http://localhost", "https://localhost", "http://127.0.0.1", "https://127.0.0.1", "http://[::1]", "https://[::1]" })
		{
			const size_t n = std::char_traits<char>::length(prefix);
			if (o.compare(0, n, prefix) == 0 && (o.size() == n || o[n] == ':' || o[n] == '/')) return true;
		}
		return false;
	}

	const char* Reason(int status)
	{
		switch (status)
		{
		case 200: return "OK";
		case 202: return "Accepted";
		case 204: return "No Content";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 413: return "Payload Too Large";
		case 500: return "Internal Server Error";
		default: return "Status";
		}
	}

	void SendAll(SOCKET client, const std::string& data)
	{
		size_t sent = 0;
		while (sent < data.size())
		{
			const int n = send(client, data.data() + sent, static_cast<int>(std::min<size_t>(data.size() - sent, 64 * 1024)), 0);
			if (n <= 0) return;
			sent += static_cast<size_t>(n);
		}
	}

	void Reply(SOCKET client, const HttpServer::Response& response, const std::string& extraHeaders = {})
	{
		std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " + Reason(response.status) + "\r\n";
		if (!response.body.empty()) head += "Content-Type: " + response.contentType + "\r\n";
		head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
		head += "Cache-Control: no-store\r\nConnection: close\r\n" + extraHeaders + "\r\n";
		SendAll(client, head + response.body);
	}

	class SocketStream final : public HttpServer::Stream
	{
	public:
		explicit SocketStream(SOCKET socket) : client(socket) {}

		bool Begin() override
		{
			if (started) return true;
			started = true;
			SendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
			return true;
		}

		bool Event(const std::string& data) override
		{
			if (!started) Begin();
			SendAll(client, "event: message\r\ndata: " + data + "\r\n\r\n");
			return true;
		}

		bool Started() const { return started; }

	private:
		SOCKET client;
		bool started = false;
	};

	void Handle(SOCKET client)
	{
		DWORD timeout = 15000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

		std::string data;
		char buffer[8192];
		size_t headerEnd = std::string::npos;
		while (headerEnd == std::string::npos)
		{
			const int n = recv(client, buffer, sizeof(buffer), 0);
			if (n <= 0) return;
			data.append(buffer, static_cast<size_t>(n));
			headerEnd = data.find("\r\n\r\n");
			if (headerEnd == std::string::npos && data.size() > kMaxHeader) return;
		}

		const std::string headers = "\r\n" + data.substr(0, headerEnd);
		const size_t lineEnd = data.find("\r\n");
		const std::string requestLine = data.substr(0, lineEnd);
		const size_t sp1 = requestLine.find(' ');
		const size_t sp2 = requestLine.find(' ', sp1 + 1);
		if (sp1 == std::string::npos || sp2 == std::string::npos)
		{
			Reply(client, { 400, "text/plain", "Bad request line" });
			return;
		}
		const std::string method = requestLine.substr(0, sp1);
		std::string path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
		if (const size_t q = path.find('?'); q != std::string::npos) path.resize(q);

		if (!OriginAllowed(HeaderValue(headers, "Origin")))
		{
			Reply(client, { 403, "text/plain", "Origin not allowed" });
			return;
		}

		size_t length = 0;
		const std::string lengthText = HeaderValue(headers, "Content-Length");
		if (!lengthText.empty())
		{
			try { length = static_cast<size_t>(std::stoull(lengthText)); }
			catch (...) { Reply(client, { 400, "text/plain", "Bad Content-Length" }); return; }
		}
		if (length > kMaxBody)
		{
			Reply(client, { 413, "text/plain", "Body too large" });
			return;
		}
		std::string body = data.substr(headerEnd + 4);
		while (body.size() < length)
		{
			const int n = recv(client, buffer, sizeof(buffer), 0);
			if (n <= 0) return;
			body.append(buffer, static_cast<size_t>(n));
		}
		body.resize(length);

		HttpServer::Request request{ method, path, std::move(body), HeaderValue(headers, "Authorization"), HeaderValue(headers, "Accept") };
		SocketStream stream(client);
		HttpServer::Response response;
		try
		{
			response = handler(request, stream);
		}
		catch (const std::exception& error)
		{
			McpLog::Write("HTTP handler error: %s", error.what());
			response = { 500, "text/plain", "Internal error" };
		}
		if (response.streamed || stream.Started()) return;
		std::string extra = response.status == 405 ? "Allow: POST\r\n" : "";
		if (response.status == 401) extra += "WWW-Authenticate: Bearer\r\n";
		Reply(client, response, extra);
	}

	void AcceptLoop()
	{
		while (running)
		{
			SOCKET client = accept(listener, nullptr, nullptr);
			if (client == INVALID_SOCKET)
			{
				if (!running) break;
				continue;
			}
			Handle(client);
			shutdown(client, SD_BOTH);
			closesocket(client);
		}
	}
}

bool HttpServer::Start(uint16_t port, Handler requestHandler)
{
	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		McpLog::Write("WSAStartup failed.");
		return false;
	}
	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET)
	{
		McpLog::Write("socket() failed: %d", WSAGetLastError());
		WSACleanup();
		return false;
	}
	BOOL exclusive = TRUE;
	setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR || listen(listener, 8) == SOCKET_ERROR)
	{
		McpLog::Write("Cannot listen on 127.0.0.1:%u (error %d). Is the port in use? Change Port in SC4MCP.ini.", port, WSAGetLastError());
		closesocket(listener);
		listener = INVALID_SOCKET;
		WSACleanup();
		return false;
	}
	handler = std::move(requestHandler);
	running = true;
	worker = std::thread(AcceptLoop);
	McpLog::Write("MCP server listening on http://127.0.0.1:%u/mcp", port);
	return true;
}

void HttpServer::Stop()
{
	if (!running.exchange(false)) return;
	closesocket(listener);
	listener = INVALID_SOCKET;
	if (worker.joinable()) worker.join();
	WSACleanup();
	McpLog::Write("MCP server stopped.");
}
