// SC4MCP - MCP JSON-RPC protocol (Streamable HTTP transport; server-sent events only for tool progress).
#pragma once
#include "HttpServer.h"

#include <string>

namespace McpServer
{
	// Bearer token required on /mcp; empty = no authentication.
	void Configure(const std::string& authToken);

	HttpServer::Response HandleHttp(const HttpServer::Request& request, HttpServer::Stream& stream);
}
