// SC4MCP - minimal HTTP/1.1 server on 127.0.0.1 for the MCP Streamable HTTP transport.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace HttpServer
{
	struct Response
	{
		int status = 200;
		std::string contentType = "application/json";
		std::string body;
	};

	// handler(method, path, body) runs on the accept thread, one request at a time.
	using Handler = std::function<Response(const std::string& method, const std::string& path, const std::string& body)>;

	bool Start(uint16_t port, Handler handler);
	void Stop();
}
