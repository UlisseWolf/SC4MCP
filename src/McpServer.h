// SC4MCP - MCP JSON-RPC protocol (Streamable HTTP transport; server-sent events only for tool progress).
#pragma once
#include "HttpServer.h"

#include <cstdint>
#include <string>

namespace McpServer
{
	// Bearer token required on /mcp (empty = no authentication), and the port shown on the /setup page.
	void Configure(const std::string& authToken, uint16_t port);

	HttpServer::Response HandleHttp(const HttpServer::Request& request, HttpServer::Stream& stream);
}
