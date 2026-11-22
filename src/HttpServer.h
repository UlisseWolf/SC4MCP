// SC4MCP - minimal HTTP/1.1 server on 127.0.0.1 for the MCP Streamable HTTP transport.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace HttpServer
{
	struct Request
	{
		std::string method;
		std::string path;
		std::string body;
		std::string authorization; // Authorization header, as sent
		std::string accept;        // Accept header, as sent
	};

	struct Response
	{
		int status = 200;
		std::string contentType = "application/json";
		std::string body;
		bool streamed = false;     // the handler already sent the whole response through the Stream
	};

	// Server-sent events on the current connection: Begin() sends the headers, Event() one "message" event.
	class Stream
	{
	public:
		virtual ~Stream() = default;
		virtual bool Begin() = 0;
		virtual bool Event(const std::string& data) = 0;
		virtual bool Started() const = 0;
	};

	// handler(request, stream) runs on a connection thread; up to 16 connections are served at the same time, so a
	// client can answer a request of the server (elicitation) while a tool call waits for it.
	using Handler = std::function<Response(const Request& request, Stream& stream)>;

	bool Start(uint16_t port, Handler handler);
	void Stop();
}
