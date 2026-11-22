// SC4MCP - DLL director: reads SC4MCP.ini, registers the tick service and starts the MCP HTTP server.
#include "HttpServer.h"
#include "MainThreadQueue.h"
#include "McpLog.h"
#include "McpServer.h"
#include "TickService.h"
#include "Tools.h"

#include "cIGZCOM.h"
#include "cIGZFrameWork.h"
#include "cRZCOMDllDirector.h"

#include <windows.h>

namespace
{
	constexpr uint32_t kMcpDirectorID = 0xC4511A4D;

	struct Settings
	{
		uint16_t port = 8765;
		bool enabled = true;
		bool allowControl = true;
		bool allowCheats = true;
	};

	Settings ReadSettings()
	{
		Settings settings;
		const std::wstring ini = (McpLog::DllFolder() / L"SC4MCP.ini").wstring();
		const int port = GetPrivateProfileIntW(L"SC4MCP", L"Port", settings.port, ini.c_str());
		if (port > 0 && port < 65536) settings.port = static_cast<uint16_t>(port);
		settings.enabled = GetPrivateProfileIntW(L"SC4MCP", L"Enabled", 1, ini.c_str()) != 0;
		settings.allowControl = GetPrivateProfileIntW(L"SC4MCP", L"AllowControl", 1, ini.c_str()) != 0;
		settings.allowCheats = GetPrivateProfileIntW(L"SC4MCP", L"AllowCheats", 1, ini.c_str()) != 0;
		return settings;
	}
}

class McpDllDirector final : public cRZCOMDllDirector
{
public:
	uint32_t GetDirectorID() const override { return kMcpDirectorID; }

	bool OnStart(cIGZCOM* pCOM) override
	{
		McpLog::Open();
		settings = ReadSettings();
		McpLog::Write("SC4MCP 1.0.0 loaded. Port=%u Enabled=%d AllowControl=%d AllowCheats=%d",
			settings.port, settings.enabled, settings.allowControl, settings.allowCheats);
		framework = pCOM->FrameWork();
		if (framework->GetState() < cIGZFrameWork::kStatePreAppInit) framework->AddHook(this);
		else PostAppInit();
		return true;
	}

	bool PostAppInit() override
	{
		if (!settings.enabled)
		{
			McpLog::Write("Disabled in SC4MCP.ini (Enabled=0).");
			return true;
		}
		Tools::Configure(settings.allowControl, settings.allowCheats);
		if (!tick.Register(framework))
		{
			McpLog::Write("Tick service registration failed: the MCP server is not started.");
			return true;
		}
		HttpServer::Start(settings.port, McpServer::HandleHttp);
		return true;
	}

	bool PostAppShutdown() override
	{
		MainThreadQueue::Close();
		HttpServer::Stop();
		tick.Unregister();
		return true;
	}

private:
	cIGZFrameWork* framework = nullptr;
	TickService tick;
	Settings settings;
};

cRZCOMDllDirector* RZGetCOMDllDirector()
{
	static McpDllDirector sDirector;
	return &sDirector;
}
