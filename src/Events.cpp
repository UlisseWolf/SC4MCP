// SC4MCP - server-to-client notifications (see Events.h).
#include "Events.h"
#include "McpLog.h"
#include "ToolsCommon.h"

#include "cISC4BudgetSimulator.h"
#include "cISC4City.h"
#include "cISC4PowerSimulator.h"
#include "cISC4ResidentialSimulator.h"
#include "cISC4Simulator.h"
#include "cRZBaseString.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	std::mutex mutex;
	std::condition_variable wake;
	std::vector<HttpServer::Stream*> streams;
	std::set<std::string> subscriptions;
	std::atomic<bool> stopping{ false };
	std::thread monitor;
	bool monitorRunning = false;
	int minLevel = 1; // info

	int LevelRank(const std::string& level)
	{
		static const char* const levels[] = { "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency" };
		for (int i = 0; i < 8; ++i) if (level == levels[i]) return i;
		return 1;
	}

	void Broadcast(const json& message)
	{
		const std::string text = message.dump();
		std::lock_guard<std::mutex> lock(mutex);
		for (HttpServer::Stream* stream : streams) stream->Event(text);
	}

	void Log(const std::string& level, const json& data)
	{
		if (LevelRank(level) < minLevel) return;
		Broadcast(json{ {"jsonrpc", "2.0"}, {"method", "notifications/message"}, {"params", { {"level", level}, {"logger", "sc4"}, {"data", data} }} });
	}

	json Sample()
	{
		cISC4City* city = City();
		if (!city || !city->GetSimulator()) return json{ {"city", false} };
		json out{ {"city", true}, {"date", DateOf(city->GetSimulator())} };
		cRZBaseString name;
		city->GetCityName(name);
		out["name"] = name.ToChar();
		if (cISC4ResidentialSimulator* residential = city->GetResidentialSimulator()) out["population"] = residential->GetPopulation();
		if (cISC4BudgetSimulator* budget = city->GetBudgetSimulator())
		{
			out["funds"] = budget->GetTotalFunds();
			out["monthly_balance"] = static_cast<int64_t>(budget->GetTotalMonthlyIncome()) - static_cast<int64_t>(budget->GetTotalMonthlyExpense());
		}
		if (cISC4PowerSimulator* power = city->GetPowerSimulator())
		{
			out["power_production"] = power->GetMonthlyProduction();
			out["power_consumption"] = power->GetMonthlyConsumption();
			out["unpowered"] = power->GetUnpoweredConsumerCount();
		}
		return out;
	}

	void MonitorLoop()
	{
		json last;
		int lastMonth = -1;
		while (!stopping)
		{
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait_for(lock, std::chrono::seconds(2), [] { return stopping.load() || streams.empty(); });
				if (stopping || streams.empty()) { monitorRunning = false; return; }
			}
			bool isError = false;
			json now = OnGameThread(Sample, isError);
			if (isError) continue;
			const bool hadCity = last.is_object() && last.value("city", false);
			const bool hasCity = now.value("city", false);
			if (hadCity != hasCity && last.is_object()) Log("notice", json{ {"event", hasCity ? "city_opened" : "city_closed"}, {"name", now.value("name", std::string())} });
			if (!hasCity) { last = now; lastMonth = -1; continue; }

			const int month = now["date"]["year"].get<int>() * 12 + now["date"]["month"].get<int>();
			if (lastMonth >= 0 && month != lastMonth)
			{
				Log("info", json{ {"event", "new_month"}, {"date", now["date"]["text"]}, {"population", now.value("population", 0)},
					{"funds", now.value("funds", 0LL)}, {"monthly_balance", now.value("monthly_balance", 0LL)} });
				std::vector<std::string> uris;
				{
					std::lock_guard<std::mutex> lock(mutex);
					uris.assign(subscriptions.begin(), subscriptions.end());
				}
				for (const std::string& uri : uris)
				{
					Broadcast(json{ {"jsonrpc", "2.0"}, {"method", "notifications/resources/updated"}, {"params", { {"uri", uri} }} });
				}
				if (now.value("funds", 0LL) < 0) Log("warning", json{ {"event", "negative_funds"}, {"funds", now["funds"]} });
				else if (now.value("monthly_balance", 0LL) < 0) Log("notice", json{ {"event", "monthly_deficit"}, {"monthly_balance", now["monthly_balance"]} });
				const int64_t production = now.value("power_production", 0LL), consumption = now.value("power_consumption", 0LL);
				if (production > 0 && consumption >= production) Log("warning", json{ {"event", "power_at_full_load"}, {"production", production}, {"consumption", consumption} });
				const int64_t unpowered = now.value("unpowered", 0LL), before = last.value("unpowered", 0LL);
				if (unpowered > before && unpowered > 0) Log("warning", json{ {"event", "unpowered_buildings"}, {"count", unpowered}, {"previous", before} });
			}
			lastMonth = month;
			last = now;
		}
		std::lock_guard<std::mutex> lock(mutex);
		monitorRunning = false;
	}
}

void Events::Serve(HttpServer::Stream& stream)
{
	stream.Begin();
	{
		std::lock_guard<std::mutex> lock(mutex);
		streams.push_back(&stream);
		if (!monitorRunning)
		{
			if (monitor.joinable()) monitor.join();
			monitorRunning = true;
			monitor = std::thread(MonitorLoop);
		}
	}
	McpLog::Write("Notification stream opened.");
	while (!stopping)
	{
		std::unique_lock<std::mutex> lock(mutex);
		wake.wait_for(lock, std::chrono::seconds(15), [] { return stopping.load(); });
		if (stopping) break;
		if (!stream.Comment("keep-alive")) break;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		streams.erase(std::remove(streams.begin(), streams.end(), &stream), streams.end());
	}
	wake.notify_all();
	McpLog::Write("Notification stream closed.");
}

void Events::Subscribe(const std::string& uri)
{
	std::lock_guard<std::mutex> lock(mutex);
	subscriptions.insert(uri);
}

void Events::Unsubscribe(const std::string& uri)
{
	std::lock_guard<std::mutex> lock(mutex);
	subscriptions.erase(uri);
}

void Events::SetLevel(const std::string& level)
{
	std::lock_guard<std::mutex> lock(mutex);
	minLevel = LevelRank(level);
}

void Events::Stop()
{
	stopping = true;
	wake.notify_all();
	std::thread finished;
	{
		std::lock_guard<std::mutex> lock(mutex);
		finished = std::move(monitor);
	}
	if (finished.joinable()) finished.join();
}
