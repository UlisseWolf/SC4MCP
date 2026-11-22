// SC4MCP - runs work on the game thread.
//
// The game objects (city, simulators, budget) may only be touched from the game thread. The HTTP thread posts a job
// here and waits; the tick service (TickService.cpp) runs the queued jobs once per frame, also while the simulation
// is paused.
#pragma once
#include <chrono>
#include <functional>
#include <string>

namespace MainThreadQueue
{
	// Runs job on the game thread and waits for it. Returns false on timeout (the game is loading, minimized with the
	// frame loop stopped, or showing a modal dialog); result is then untouched.
	bool Run(const std::function<std::string()>& job, std::string& result,
		std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

	// Called by the tick service on the game thread.
	void RunPending();

	// Called at shutdown: pending and future jobs fail at once.
	void Close();
}
