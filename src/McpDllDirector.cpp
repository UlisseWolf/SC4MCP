// SC4MCP - DLL director: reads SC4MCP.ini, registers the tick service and starts the MCP HTTP server.
#include "Events.h"
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
#include <random>
#include <string>

namespace
{
	constexpr uint32_t kMcpDirectorID = 0xC4511A4D;

	struct Settings
	{
		uint16_t port = 8765;
		bool enabled = true;
		bool allowControl = true;
		bool allowCheats = true;
		bool allowBuild = true;
		bool readOnly = false;
		bool confirmChanges = true;
		bool allowDangerous = false;
		bool tokenGenerated = false;
		std::string authToken;
	};

	// 32 random hex characters (std::random_device uses the system's cryptographic generator with MSVC).
	std::string RandomToken()
	{
		std::random_device random;
		static const char digits[] = "0123456789abcdef";
		std::string token;
		for (int i = 0; i < 32; ++i) token += digits[random() & 15];
		return token;
	}

	Settings ReadSettings()
	{
		Settings settings;
		const std::wstring ini = (McpLog::DllFolder() / L"SC4MCP.ini").wstring();
		const int port = GetPrivateProfileIntW(L"SC4MCP", L"Port", settings.port, ini.c_str());
		if (port > 0 && port < 65536) settings.port = static_cast<uint16_t>(port);
		settings.enabled = GetPrivateProfileIntW(L"SC4MCP", L"Enabled", 1, ini.c_str()) != 0;
		settings.allowControl = GetPrivateProfileIntW(L"SC4MCP", L"AllowControl", 1, ini.c_str()) != 0;
		settings.allowCheats = GetPrivateProfileIntW(L"SC4MCP", L"AllowCheats", 1, ini.c_str()) != 0;
		settings.allowBuild = GetPrivateProfileIntW(L"SC4MCP", L"AllowBuild", 1, ini.c_str()) != 0;
		settings.readOnly = GetPrivateProfileIntW(L"SC4MCP", L"ReadOnly", 0, ini.c_str()) != 0;
		settings.confirmChanges = GetPrivateProfileIntW(L"SC4MCP", L"ConfirmChanges", 1, ini.c_str()) != 0;
		settings.allowDangerous = GetPrivateProfileIntW(L"SC4MCP", L"AllowDangerousCommands", 0, ini.c_str()) != 0;
		if (settings.readOnly) settings.allowControl = settings.allowCheats = settings.allowBuild = settings.allowDangerous = false;
		char token[256]{};
		GetPrivateProfileStringA("SC4MCP", "AuthToken", "", token, sizeof(token), (McpLog::DllFolder() / L"SC4MCP.ini").string().c_str());
		settings.authToken = token;
		while (!settings.authToken.empty() && (settings.authToken.back() == ' ' || settings.authToken.back() == '\t')) settings.authToken.pop_back();
		// AuthToken=auto: generate a token once and write it back, so it stays the same at every start.
		if (_stricmp(settings.authToken.c_str(), "auto") == 0)
		{
			settings.authToken = RandomToken();
			const std::wstring value(settings.authToken.begin(), settings.authToken.end());
			WritePrivateProfileStringW(L"SC4MCP", L"AuthToken", value.c_str(), ini.c_str());
			settings.tokenGenerated = true;
		}
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
		McpLog::Write("SC4MCP 1.4.0 loaded. Port=%u Enabled=%d ReadOnly=%d AllowControl=%d AllowCheats=%d AllowBuild=%d ConfirmChanges=%d AuthToken=%s",
			settings.port, settings.enabled, settings.readOnly, settings.allowControl, settings.allowCheats, settings.allowBuild,
			settings.confirmChanges, settings.authToken.empty() ? "off" : "on");
		if (settings.tokenGenerated) McpLog::Write("AuthToken=auto: a new token was generated and written to SC4MCP.ini; http://127.0.0.1:%u/setup shows the client configurations with it.", settings.port);
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
		Tools::Configure(settings.allowControl, settings.allowCheats, settings.allowBuild, settings.confirmChanges, settings.allowDangerous);
		McpServer::Configure(settings.authToken, settings.port);
		if (!tick.Register(framework))
		{
			McpLog::Write("Tick service registration failed: the MCP server is not started.");
			return true;
		}
		if (HttpServer::Start(settings.port, McpServer::HandleHttp))
		{
			McpLog::Write("Client configurations: http://127.0.0.1:%u/setup", settings.port);
		}
		return true;
	}

	bool PostAppShutdown() override
	{
		Events::Stop();
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
