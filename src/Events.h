// SC4MCP - server-to-client notifications on the standalone stream (GET /mcp with Accept: text/event-stream).
//
// While at least one client keeps the stream open, a monitor thread polls the city every 2 seconds and sends:
//  - notifications/message (logging): a new game month with its key figures, and warnings (funds below zero, a monthly
//    deficit, power plants at full load, unpowered buildings, a city closed or opened);
//  - notifications/resources/updated for the resources the client subscribed to, at every new month.
#pragma once
#include "HttpServer.h"

#include <string>

namespace Events
{
	// Serves one stream until the client disconnects or the server stops. Runs on the connection thread.
	void Serve(HttpServer::Stream& stream);

	void Subscribe(const std::string& uri);
	void Unsubscribe(const std::string& uri);

	// Lowest level sent: debug, info, notice, warning, error (logging/setLevel).
	void SetLevel(const std::string& level);

	void Stop();
}
