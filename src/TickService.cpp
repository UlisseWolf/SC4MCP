// SC4MCP - framework tick service: runs MainThreadQueue jobs once per frame on the game thread.
#include "TickService.h"
#include "MainThreadQueue.h"
#include "McpLog.h"

#include "cIGZFrameWork.h"
#include "cIGZUnknown.h"


bool TickService::Register(cIGZFrameWork* fw)
{
	framework = fw;
	if (!framework) return false;
	if (!framework->AddSystemService(this))
	{
		McpLog::Write("Tick service: AddSystemService failed.");
		return false;
	}
	if (!framework->AddToTick(this))
	{
		McpLog::Write("Tick service: AddToTick failed.");
		framework->RemoveSystemService(this);
		return false;
	}
	running = true;
	return true;
}

void TickService::Unregister()
{
	if (!framework) return;
	framework->RemoveFromTick(this);
	framework->RemoveSystemService(this);
	running = false;
	framework = nullptr;
}

bool TickService::QueryInterface(uint32_t riid, void** ppvObj)
{
	if (riid == GZIID_cIGZSystemService)
	{
		*ppvObj = static_cast<cIGZSystemService*>(this);
		AddRef();
		return true;
	}
	if (riid == GZIID_cIGZUnknown)
	{
		*ppvObj = static_cast<cIGZUnknown*>(this);
		AddRef();
		return true;
	}
	return false;
}

uint32_t TickService::AddRef() { return ++refCount; }
uint32_t TickService::Release() { return refCount > 0 ? --refCount : 0; }

bool TickService::OnTick(uint32_t)
{
	MainThreadQueue::RunPending();
	return true;
}
