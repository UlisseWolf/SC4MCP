// SC4MCP - framework tick service: runs MainThreadQueue jobs once per frame on the game thread.
#pragma once
#include "cIGZSystemService.h"

class cIGZFrameWork;

class TickService final : public cIGZSystemService
{
public:
	static constexpr uint32_t kServiceID = 0x5C4D3C00;

	bool Register(cIGZFrameWork* framework);
	void Unregister();

	bool QueryInterface(uint32_t riid, void** ppvObj) override;
	uint32_t AddRef() override;
	uint32_t Release() override;

	uint32_t GetServiceID() override { return serviceID; }
	void SetServiceID(uint32_t id) override { serviceID = id; }
	int32_t GetServicePriority() override { return 0; }
	bool IsServiceRunning() override { return running; }
	void SetServiceRunning(bool value) override { running = value; }
	bool Init() override { return true; }
	bool Shutdown() override { return true; }
	bool OnTick(uint32_t elapsed) override;
	bool OnIdle(uint32_t) override { return true; }
	int32_t GetServiceTickPriority() override { return 0; }

private:
	cIGZFrameWork* framework = nullptr;
	uint32_t serviceID = kServiceID;
	uint32_t refCount = 0;
	bool running = false;
};
