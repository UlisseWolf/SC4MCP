// SC4MCP - MCP tools.
//
// Every tool that reads or changes the city runs on the game thread through MainThreadQueue. wait_months is the only
// tool that waits on the HTTP thread: it lets the simulation run and polls the date.
#include "Tools.h"
#include "MainThreadQueue.h"
#include "McpLog.h"

#include "cIGZCheatCodeManager.h"
#include "cIGZString.h"
#include "cISC4App.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4City.h"
#include "cISC4Demand.h"
#include "cISC4DemandSimulator.h"
#include "cISC4DepartmentBudget.h"
#include "cISC4LineItem.h"
#include "cISC4Occupant.h"
#include "cISC4OccupantFilter.h"
#include "cISC4OccupantManager.h"
#include "cISC4ResidentialSimulator.h"
#include "cISC4Simulator.h"
#include "cISCPropertyHolder.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"
#include "SC4Percentage.h"
#include "SC4Rect.h"
#include "SC4Vector.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

using nlohmann::json;

namespace
{
	bool controlAllowed = true;
	bool cheatsAllowed = true;

	constexpr uint32_t kOccupantTypeBuilding = 0x278128A0;

	std::string Hex(uint32_t value)
	{
		char text[16];
		std::snprintf(text, sizeof(text), "0x%08X", value);
		return text;
	}

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	cISC4City* City()
	{
		cISC4AppPtr app;
		return app ? app->GetCity() : nullptr;
	}

	json NoCity()
	{
		return json{ {"city_loaded", false}, {"message", "No city is open: open a city in SimCity 4 first."} };
	}

	// Runs work on the game thread and parses its JSON result.
	json OnGameThread(const std::function<json()>& work, bool& isError)
	{
		std::string text;
		const bool ran = MainThreadQueue::Run([&]() -> std::string { return work().dump(); }, text);
		if (!ran)
		{
			isError = true;
			return json{ {"error", "The game did not answer in time (loading, minimized or a dialog is open)."} };
		}
		json result = json::parse(text, nullptr, false);
		if (result.is_discarded())
		{
			isError = true;
			return json{ {"error", "Invalid result from the game thread."} };
		}
		if (result.contains("error")) isError = true;
		return result;
	}

	// ------------------------------------------------------------------------------------------------- readers

	json DateOf(cISC4Simulator* simulator)
	{
		uint32_t year = 0, month = 0, day = 0, dayOfYear = 0, weekDay = 0;
		simulator->GetSimDate(&year, &month, &day, &dayOfYear, &weekDay);
		char text[32];
		std::snprintf(text, sizeof(text), "%04u-%02u-%02u", year, month, day);
		return json{ {"year", year}, {"month", month}, {"day", day}, {"text", text}, {"day_number", simulator->GetSimDateNumber()} };
	}

	json DemandOf(cISC4City* city)
	{
		json out = json::object();
		cISC4DemandSimulator* demand = city->GetDemandSimulator();
		if (!demand) return out;
		static const std::pair<uint32_t, const char*> kDemands[] = {
			{0x1010, "R$"}, {0x1020, "R$$"}, {0x1030, "R$$$"},
			{0x3110, "Cs$"}, {0x3120, "Cs$$"}, {0x3130, "Cs$$$"}, {0x3320, "Co$$"}, {0x3330, "Co$$$"},
			{0x4100, "I-R"}, {0x4200, "I-D"}, {0x4300, "I-M"}, {0x4400, "I-HT"} };
		for (const auto& [id, label] : kDemands)
		{
			// SC4 keeps the I-M and I-HT demand under index 0x20000; the others under 0.
			const uint32_t index = (id == 0x4300 || id == 0x4400) ? 0x20000 : 0;
			const cISC4Demand* d = demand->GetDemand(id, index);
			if (!d) continue;
			const SC4Percentage* cap = d->GetDemandCap();
			out[label] = json{ {"id", Hex(id)}, {"demand", d->QueryDemandValue()}, {"supply", d->QuerySupplyValue()},
				{"active_demand", d->QueryActiveDemandValue()}, {"cap", cap ? cap->percentage : 0.0f} };
		}
		return out;
	}

	class BuildingFilter final : public cISC4OccupantFilter
	{
	public:
		bool QueryInterface(uint32_t riid, void** ppvObj) override
		{
			if (riid == GZIID_cISC4OccupantFilter || riid == GZIID_cIGZUnknown)
			{
				*ppvObj = static_cast<cISC4OccupantFilter*>(this);
				AddRef();
				return true;
			}
			return false;
		}
		uint32_t AddRef() override { return ++refCount; }
		uint32_t Release() override { return refCount > 0 ? --refCount : 0; }
		bool IsOccupantIncluded(cISC4Occupant* occupant) override { return occupant && occupant->GetType() == static_cast<int32_t>(kOccupantTypeBuilding); }
		bool IsOccupantTypeIncluded(uint32_t type) override { return type == kOccupantTypeBuilding; }
		bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return true; }
	private:
		uint32_t refCount = 1;
	};

	struct Collected
	{
		std::unordered_set<cISC4Occupant*> seen;
		std::vector<cISC4Occupant*> buildings;
	};

	bool Collect(cISC4Occupant* occupant, void* data)
	{
		auto& collected = *static_cast<Collected*>(data);
		if (collected.seen.insert(occupant).second) collected.buildings.push_back(occupant);
		return true;
	}

	std::vector<cISC4Occupant*> AllBuildings(cISC4City* city)
	{
		Collected collected;
		cISC4OccupantManager* occupants = city->GetOccupantManager();
		if (!occupants) return {};
		BuildingFilter filter;
		occupants->IterateOccupants(Collect, &collected, nullptr, nullptr, &filter);
		return collected.buildings;
	}

	std::string BuildingName(cISC4Occupant* occupant)
	{
		cRZAutoRefCount<cISC4BuildingOccupant> building;
		if (!occupant->QueryInterface(GZIID_cISC4BuildingOccupant, building.AsPPVoid())) return {};
		cRZBaseString name;
		if (building->GetName(name) && name.Strlen() > 0) return name.ToChar();
		if (cIGZString* exemplarName = building->GetExemplarName()) return exemplarName->ToChar();
		return {};
	}

	uint32_t BuildingType(cISC4Occupant* occupant)
	{
		cRZAutoRefCount<cISC4BuildingOccupant> building;
		if (!occupant->QueryInterface(GZIID_cISC4BuildingOccupant, building.AsPPVoid())) return 0;
		return building->GetBuildingType();
	}

	struct BuildingCost
	{
		int64_t monthly = 0;
		std::vector<cISC4BudgetSimulator::BudgetItem> items;
	};

	BuildingCost CostOf(cISC4BudgetSimulator* budget, cISC4Occupant* occupant)
	{
		BuildingCost cost;
		cISCPropertyHolder* holder = occupant->AsPropertyHolder();
		if (!budget || !holder) return cost;
		SC4Vector<cISC4BudgetSimulator::BudgetItem> items;
		if (!budget->GetBudgetItemInfo(holder, items)) return cost;
		for (const auto& item : items)
		{
			cost.items.push_back(item);
			cost.monthly += item.cost;
		}
		return cost;
	}

	std::string DepartmentName(cISC4DepartmentBudget* department)
	{
		cRZBaseString name;
		if (department && department->GetDepartmentName(name) && name.Strlen() > 0) return name.ToChar();
		return {};
	}

	// ------------------------------------------------------------------------------------------------- tools

	json GetCityStatus()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		json out{ {"city_loaded", true} };
		cRZBaseString name;
		city->GetCityName(name);
		out["name"] = name.ToChar();
		if (cISC4Simulator* simulator = city->GetSimulator())
		{
			out["date"] = DateOf(simulator);
			out["paused"] = simulator->IsPaused();
			out["speed"] = simulator->GetSimSpeed();
		}
		if (cISC4ResidentialSimulator* residential = city->GetResidentialSimulator())
		{
			out["population"] = residential->GetPopulation();
		}
		if (cISC4BudgetSimulator* budget = city->GetBudgetSimulator())
		{
			out["funds"] = budget->GetTotalFunds();
			out["monthly_income"] = budget->GetTotalMonthlyIncome();
			out["monthly_expense"] = budget->GetTotalMonthlyExpense();
			out["monthly_balance"] = static_cast<int64_t>(budget->GetTotalMonthlyIncome()) - static_cast<int64_t>(budget->GetTotalMonthlyExpense());
			out["tax_income_ytd"] = budget->GetTaxIncome();
			out["borrowed"] = budget->GetTotalBorrowed();
		}
		out["demand"] = DemandOf(city);
		return out;
	}

	json GetBudget(bool perBuilding)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		if (!budget) return json{ {"error", "Budget simulator unavailable."} };

		json out{ {"funds", budget->GetTotalFunds()}, {"monthly_income", budget->GetTotalMonthlyIncome()},
			{"monthly_expense", budget->GetTotalMonthlyExpense()}, {"tax_income_ytd", budget->GetTaxIncome()},
			{"income_ytd", budget->GetYTDIncome()}, {"expenses_ytd", budget->GetYTDExpenses()},
			{"estimated_income", budget->GetEstIncome()}, {"estimated_expenses", budget->GetEstExpenses()},
			{"loan_payments_monthly", budget->GetTotalMonthlyBondPayments()} };
		if (cISC4ResidentialSimulator* residential = city->GetResidentialSimulator()) out["population"] = residential->GetPopulation();

		// Departments and line items, found through the budget items of the buildings in the city.
		struct Line { int64_t buildingCost = 0; int count = 0; };
		std::map<uint32_t, std::map<uint32_t, Line>> lines;
		json buildings = json::array();
		for (cISC4Occupant* occupant : AllBuildings(city))
		{
			const BuildingCost cost = CostOf(budget, occupant);
			if (cost.items.empty()) continue;
			for (const auto& item : cost.items)
			{
				Line& line = lines[item.departmentID][item.lineItem];
				line.buildingCost += item.cost;
				++line.count;
			}
			if (perBuilding)
			{
				json items = json::array();
				for (const auto& item : cost.items)
				{
					items.push_back(json{ {"department", Hex(item.departmentID)}, {"line", Hex(item.lineItem)},
						{"purpose", Hex(item.purpose)}, {"cost", item.cost}, {"local_funding", item.localFundingPercentage} });
				}
				buildings.push_back(json{ {"name", BuildingName(occupant)}, {"exemplar", Hex(BuildingType(occupant))},
					{"monthly_cost", cost.monthly}, {"items", items} });
			}
		}

		json departments = json::array();
		for (const auto& [departmentID, departmentLines] : lines)
		{
			cISC4DepartmentBudget* department = budget->GetDepartmentBudget(departmentID);
			json d{ {"id", Hex(departmentID)} };
			if (department)
			{
				d["name"] = DepartmentName(department);
				const SC4Percentage* funding = department->GetFundingPercentage();
				d["funding_percent"] = funding ? funding->percentage * 100.0f : 0.0f;
				d["total_expenses"] = department->GetTotalExpenses();
				d["total_income"] = department->GetTotalIncome();
				d["ideal_monthly_funding"] = department->GetIdealMonthlyFunding();
			}
			json lineArray = json::array();
			for (const auto& [lineID, line] : departmentLines)
			{
				json l{ {"line", Hex(lineID)}, {"buildings", line.count}, {"sum_of_building_costs", line.buildingCost} };
				if (department)
				{
					if (cISC4LineItem* item = department->GetLineItem(lineID))
					{
						l["full_expenses"] = item->GetFullExpenses();
						l["current_expenses"] = item->GetCurrentExpenses();
						l["income"] = item->GetIncome();
					}
				}
				lineArray.push_back(l);
			}
			d["lines"] = lineArray;
			departments.push_back(d);
		}
		out["departments"] = departments;
		if (perBuilding) out["buildings"] = buildings;
		return out;
	}

	json ListBuildings(const std::string& nameFilter, uint32_t occupantGroup, size_t limit)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		const std::string needle = Lower(nameFilter);
		json list = json::array();
		size_t total = 0;
		for (cISC4Occupant* occupant : AllBuildings(city))
		{
			if (occupantGroup != 0 && !occupant->IsOccupantGroup(occupantGroup)) continue;
			const std::string name = BuildingName(occupant);
			if (!needle.empty() && Lower(name).find(needle) == std::string::npos) continue;
			++total;
			if (list.size() >= limit) continue;
			json b{ {"name", name}, {"exemplar", Hex(BuildingType(occupant))} };
			SC4Rect<long> cells{};
			if (occupant->GetBoundingCityCells(cells))
			{
				b["cells"] = json{ {"x1", cells.topLeftX}, {"z1", cells.topLeftY}, {"x2", cells.bottomRightX}, {"z2", cells.bottomRightY} };
			}
			const BuildingCost cost = CostOf(budget, occupant);
			if (!cost.items.empty()) b["monthly_cost"] = cost.monthly;
			list.push_back(b);
		}
		return json{ {"total", total}, {"returned", list.size()}, {"buildings", list} };
	}

	json SetSpeed(int speed)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4Simulator* simulator = city->GetSimulator();
		if (!simulator) return json{ {"error", "Simulator unavailable."} };
		if (speed <= 0)
		{
			if (!simulator->IsPaused()) simulator->Pause();
		}
		else
		{
			if (simulator->IsPaused()) simulator->Resume();
			simulator->SetSimSpeed(std::clamp(speed, 1, 4));
		}
		return json{ {"paused", simulator->IsPaused()}, {"speed", simulator->GetSimSpeed()}, {"date", DateOf(simulator)} };
	}

	json RunCheat(const std::string& text)
	{
		cISC4AppPtr app;
		cIGZCheatCodeManager* cheats = app ? app->GetCheatCodeManager() : nullptr;
		if (!cheats) return json{ {"error", "Cheat code manager unavailable."} };
		cRZBaseString line(text.c_str());
		uint32_t id = 0;
		if (!cheats->DoesCheatCodeMatch(line, id))
		{
			return json{ {"error", "Unknown cheat: " + text} };
		}
		const bool sent = cheats->SendCheatNotifications(line, id);
		McpLog::Write("run_cheat: %s (0x%08X) %s", text.c_str(), id, sent ? "sent" : "not sent");
		return json{ {"cheat", text}, {"id", Hex(id)}, {"sent", sent} };
	}

	json ReadLogTail(const std::string& fileName, size_t lines)
	{
		std::ifstream file(McpLog::DllFolder() / fileName);
		if (!file) return json{ {"error", "Log file not found next to SC4MCP.dll."} };
		std::vector<std::string> all;
		std::string line;
		while (std::getline(file, line)) all.push_back(line);
		const size_t start = all.size() > lines ? all.size() - lines : 0;
		json out = json::array();
		for (size_t i = start; i < all.size(); ++i) out.push_back(all[i]);
		return json{ {"file", fileName}, {"total_lines", all.size()}, {"lines", out} };
	}

	// Lets the simulation run until `months` months have passed (or `timeout_seconds`), then pauses it.
	json WaitMonths(int months, int speed, int timeoutSeconds, bool& isError)
	{
		json start = OnGameThread([&]() -> json
		{
			cISC4City* city = City();
			if (!city || !city->GetSimulator()) return NoCity();
			json date = DateOf(city->GetSimulator());
			SetSpeed(speed);
			return date;
		}, isError);
		if (isError || !start.contains("year")) return start;
		const int startIndex = start["year"].get<int>() * 12 + start["month"].get<int>();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
		json now = start;
		while (std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			bool pollError = false;
			now = OnGameThread([&]() -> json
			{
				cISC4City* city = City();
				if (!city || !city->GetSimulator()) return NoCity();
				return DateOf(city->GetSimulator());
			}, pollError);
			if (pollError || !now.contains("year")) continue;
			if (now["year"].get<int>() * 12 + now["month"].get<int>() - startIndex >= months) break;
		}
		bool stopError = false;
		json status = OnGameThread([&]() -> json { SetSpeed(0); return GetCityStatus(); }, stopError);
		const bool reached = now.contains("year") && now["year"].get<int>() * 12 + now["month"].get<int>() - startIndex >= months;
		return json{ {"start_date", start}, {"months_requested", months}, {"reached", reached}, {"status", status} };
	}

	json Schema(json properties, json required = json::array())
	{
		return json{ {"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)} };
	}
}

void Tools::Configure(bool allowControl, bool allowCheats)
{
	controlAllowed = allowControl;
	cheatsAllowed = allowCheats;
}

json Tools::List()
{
	json tools = json::array();
	tools.push_back({ {"name", "get_city_status"},
		{"description", "Current city: name, date, pause state and speed, population, funds, monthly income/expense and the RCI demand values."},
		{"inputSchema", Schema(json::object())} });
	tools.push_back({ {"name", "get_budget"},
		{"description", "Budget of the open city: totals, every department with its funding percentage and line items (expenses as the game holds them and as the sum of the current costs of the buildings). per_building=true adds every building with its budget items."},
		{"inputSchema", Schema({ {"per_building", {{"type", "boolean"}, {"description", "Include every building with its budget items."}}} })} });
	tools.push_back({ {"name", "list_buildings"},
		{"description", "Buildings in the city with exemplar ID, city cells and monthly cost. Filter by name substring and/or occupant group (e.g. 0x1503 schools, 0x1507 health, 0x1400 power)."},
		{"inputSchema", Schema({ {"name_contains", {{"type", "string"}}}, {"occupant_group", {{"type", "string"}, {"description", "Hex occupant group, e.g. 0x1503."}}},
			{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}} })} });
	tools.push_back({ {"name", "read_log"},
		{"description", "Last lines of a log file next to the DLL: SC4SIM.log (default), SC4MCP.log or another *.log in the Plugins folder."},
		{"inputSchema", Schema({ {"file", {{"type", "string"}}}, {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}} })} });
	if (controlAllowed)
	{
		tools.push_back({ {"name", "set_speed"},
			{"description", "Pause (0) or run the simulation at speed 1 (turtle) to 4 (cheetah)."},
			{"inputSchema", Schema({ {"speed", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}}} }, json::array({ "speed" }))} });
		tools.push_back({ {"name", "wait_months"},
			{"description", "Runs the simulation for the given number of game months (at speed 1-4), then pauses and returns the city status. Waits at most timeout_seconds (default 50) of real time."},
			{"inputSchema", Schema({ {"months", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}}}, {"speed", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
				{"timeout_seconds", {{"type", "integer"}, {"minimum", 5}, {"maximum", 600}}} }, json::array({ "months" }))} });
	}
	if (cheatsAllowed)
	{
		tools.push_back({ {"name", "run_cheat"},
			{"description", "Runs a cheat code as if typed in the Ctrl+X box, including the cheats of DLL plugins (e.g. FundingReset, ScanPumps, IMZoneConvert, RemovePhantomBudgetItems Health)."},
			{"inputSchema", Schema({ {"cheat", {{"type", "string"}}} }, json::array({ "cheat" }))} });
	}
	return tools;
}

json Tools::Call(const std::string& name, const json& arguments, bool& isError)
{
	isError = false;
	try
	{
		if (name == "get_city_status") return OnGameThread(GetCityStatus, isError);
		if (name == "get_budget")
		{
			const bool perBuilding = arguments.value("per_building", false);
			return OnGameThread([perBuilding] { return GetBudget(perBuilding); }, isError);
		}
		if (name == "list_buildings")
		{
			const std::string filter = arguments.value("name_contains", std::string());
			const std::string groupText = arguments.value("occupant_group", std::string());
			const uint32_t group = groupText.empty() ? 0 : static_cast<uint32_t>(std::stoul(groupText, nullptr, 0));
			const size_t limit = static_cast<size_t>(std::clamp(arguments.value("limit", 200), 1, 2000));
			return OnGameThread([=] { return ListBuildings(filter, group, limit); }, isError);
		}
		if (name == "read_log")
		{
			std::string file = arguments.value("file", std::string("SC4SIM.log"));
			if (file.find_first_of("/\\:") != std::string::npos || file.size() < 5 || Lower(file.substr(file.size() - 4)) != ".log")
			{
				isError = true;
				return json{ {"error", "file must be the name of a .log file in the Plugins folder."} };
			}
			const size_t lines = static_cast<size_t>(std::clamp(arguments.value("lines", 100), 1, 1000));
			json out = ReadLogTail(file, lines);
			if (out.contains("error")) isError = true;
			return out;
		}
		if (controlAllowed && name == "set_speed")
		{
			const int speed = arguments.at("speed").get<int>();
			return OnGameThread([speed] { return SetSpeed(speed); }, isError);
		}
		if (controlAllowed && name == "wait_months")
		{
			const int months = std::clamp(arguments.at("months").get<int>(), 1, 120);
			const int speed = std::clamp(arguments.value("speed", 4), 1, 4);
			const int timeout = std::clamp(arguments.value("timeout_seconds", 50), 5, 600);
			return WaitMonths(months, speed, timeout, isError);
		}
		if (cheatsAllowed && name == "run_cheat")
		{
			const std::string cheat = arguments.at("cheat").get<std::string>();
			return OnGameThread([cheat] { return RunCheat(cheat); }, isError);
		}
	}
	catch (const std::exception& error)
	{
		isError = true;
		return json{ {"error", std::string("Invalid arguments: ") + error.what()} };
	}
	isError = true;
	return json{ {"error", "Unknown tool: " + name} };
}
