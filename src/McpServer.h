// SC4MCP - MCP JSON-RPC protocol (Streamable HTTP transport, no server-sent events).
#pragma once
#include "HttpServer.h"

namespace McpServer
{
	HttpServer::Response HandleHttp(const std::string& method, const std::string& path, const std::string& body);
}
