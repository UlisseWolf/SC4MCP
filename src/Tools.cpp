// SC4MCP - MCP tools: registry, the core tools, resources and prompts.
//
// Every tool that reads or changes the city runs on the game thread through MainThreadQueue (ToolsCommon::OnGameThread).
// wait_months and set_speed also wait on the HTTP thread: the game applies a speed change on its next frames, so the
// tool reads the result back afterwards. The other tools are in ToolsRead.cpp (read-only) and ToolsControl.cpp.
#include "Tools.h"
#include "ToolsCommon.h"
#include "McpLog.h"

#include "cIGZCheatCodeManager.h"
#include "cISC4App.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4City.h"
#include "cISC4Demand.h"
#include "cISC4DemandSimulator.h"
#include "cISC4DepartmentBudget.h"
#include "cISC4LineItem.h"
#include "cISC4Occupant.h"
#include "cISC4ResidentialSimulator.h"
#include "cISC4Simulator.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"
#include "SC4Percentage.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	bool controlAllowed = true;
	bool cheatsAllowed = true;
	bool buildAllowed = true;
	bool confirmChanges = true;

	bool Allowed(Permission permission)
	{
		switch (permission)
		{
		case Permission::Control: return controlAllowed;
		case Permission::Cheats: return cheatsAllowed;
		case Permission::Build: return buildAllowed;
		default: return true;
		}
	}

	const std::vector<ToolDef>& Registry()
	{
		static const std::vector<ToolDef> tools = []
		{
			std::vector<ToolDef> all;
			AddCoreTools(all);
			AddReadTools(all);
			AddControlTools(all);
			AddDiagnosticTools(all);
			AddExtraTools(all);
			return all;
		}();
		return tools;
	}

	// ------------------------------------------------------------------------------------------------- demand

	struct DemandType { uint32_t id; const char* label; };
	constexpr DemandType kDemands[] = {
		{0x1010, "R$"}, {0x1020, "R$$"}, {0x1030, "R$$$"},
		{0x3110, "Cs$"}, {0x3120, "Cs$$"}, {0x3130, "Cs$$$"}, {0x3320, "Co$$"}, {0x3330, "Co$$$"},
		{0x4100, "I-R"}, {0x4200, "I-D"}, {0x4300, "I-M"}, {0x4400, "I-HT"} };

	// The demand simulator keeps the city totals under index 0x20000 (the index the game's own demand UI reads);
	// older SC4MCP versions read index 0 for most types, which is not the total.
	constexpr uint32_t kTotalsIndex = 0x20000;

	const cISC4Demand* TotalDemand(cISC4DemandSimulator* simulator, uint32_t id)
	{
		const cISC4Demand* demand = simulator->GetDemand(id, kTotalsIndex);
		return demand ? demand : simulator->GetDemand(id, 0);
	}

	json DemandSummary(cISC4City* city)
	{
		json out = json::object();
		cISC4DemandSimulator* simulator = city->GetDemandSimulator();
		if (!simulator) return out;
		for (const auto& type : kDemands)
		{
			if (const cISC4Demand* demand = TotalDemand(simulator, type.id)) out[type.label] = demand->QueryDemandValue();
		}
		return out;
	}

	// ------------------------------------------------------------------------------------------------- core tools

	json GetCityStatus()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		json out{ {"city_loaded", true} };
		cRZBaseString name;
		city->GetCityName(name);
		out["name"] = name.ToChar();
		out["size_cells"] = json{ {"x", city->CellCountX()}, {"z", city->CellCountZ()} };
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
		out["demand"] = DemandSummary(city);
		return out;
	}

	json GetBudget(bool perBuilding)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		if (!budget) return Error("Budget simulator unavailable.");

		json out{ {"funds", budget->GetTotalFunds()}, {"monthly_income", budget->GetTotalMonthlyIncome()},
			{"monthly_expense", budget->GetTotalMonthlyExpense()}, {"tax_income_ytd", budget->GetTaxIncome()},
			{"income_ytd", budget->GetYTDIncome()}, {"expenses_ytd", budget->GetYTDExpenses()},
			{"estimated_income", budget->GetEstIncome()}, {"estimated_expenses", budget->GetEstExpenses()},
			{"loan_payments_monthly", budget->GetTotalMonthlyBondPayments()}, {"borrowed", budget->GetTotalBorrowed()},
			{"borrowing_limit", budget->GetCurrentBorrowingLimit()} };
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
				Line& line = lines[item.department][item.line];
				line.buildingCost += item.cost;
				++line.count;
			}
			if (perBuilding)
			{
				json items = json::array();
				for (const auto& item : cost.items)
				{
					items.push_back(json{ {"department", Hex(item.department)}, {"line", Hex(item.line)},
						{"purpose", Hex(item.purpose)}, {"cost", item.cost}, {"local_funding", item.localFunding} });
				}
				buildings.push_back(json{ {"name", BuildingName(occupant)}, {"exemplar", Hex(BuildingType(occupant))},
					{"cells", BuildingCells(occupant)}, {"monthly_cost", cost.monthly}, {"items", items} });
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
				const SC4Percentage* maximum = department->GetMaxAllowedFundingPercentage();
				if (maximum) d["max_funding_percent"] = maximum->percentage * 100.0f;
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

	json ListBuildings(const std::string& nameFilter, uint32_t occupantGroup, size_t limit, size_t offset)
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
			if (total <= offset || list.size() >= limit) continue;
			json b{ {"name", name}, {"exemplar", Hex(BuildingType(occupant))}, {"cells", BuildingCells(occupant)} };
			const BuildingCost cost = CostOf(budget, occupant);
			if (!cost.items.empty()) b["monthly_cost"] = cost.monthly;
			list.push_back(b);
		}
		json out{ {"total", total}, {"offset", offset}, {"returned", list.size()}, {"buildings", list} };
		if (offset + list.size() < total) out["next_offset"] = offset + list.size();
		return out;
	}

	// Asks the simulator for a speed; the game applies it on its next frames.
	json RequestSpeed(int speed)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4Simulator* simulator = city->GetSimulator();
		if (!simulator) return Error("Simulator unavailable.");
		if (speed <= 0)
		{
			if (!simulator->IsPaused()) simulator->Pause();
		}
		else
		{
			simulator->SetSimSpeed(std::clamp(speed, 1, 4));
			if (simulator->IsPaused()) simulator->Resume();
		}
		return json{ {"requested", true} };
	}

	json SpeedState()
	{
		cISC4City* city = City();
		if (!city || !city->GetSimulator()) return NoCity();
		cISC4Simulator* simulator = city->GetSimulator();
		return json{ {"paused", simulator->IsPaused()}, {"speed", simulator->GetSimSpeed()}, {"date", DateOf(simulator)} };
	}

	// Sets the speed and reads it back once the game has applied it (it reported the old speed before 1.1.0).
	json SetSpeed(int speed, bool& isError)
	{
		json state;
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			json request = OnGameThread([speed] { return RequestSpeed(speed); }, isError);
			if (isError) return request;
			for (int wait = 0; wait < 10; ++wait)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				bool readError = false;
				state = OnGameThread(SpeedState, readError);
				if (readError) continue;
				const bool paused = state.value("paused", false);
				const bool applied = speed <= 0 ? paused : (!paused && state.value("speed", -1) == speed);
				if (applied)
				{
					state["requested_speed"] = speed;
					state["applied"] = true;
					return state;
				}
			}
		}
		state["requested_speed"] = speed;
		state["applied"] = false;
		state["note"] = "The game did not apply the speed within 2 seconds (a dialog or the game's own pause may hold it).";
		return state;
	}

	json RunCheat(const std::string& text)
	{
		cISC4AppPtr app;
		cIGZCheatCodeManager* cheats = app ? app->GetCheatCodeManager() : nullptr;
		if (!cheats) return Error("Cheat code manager unavailable.");
		cRZBaseString line(text.c_str());
		uint32_t id = 0;
		if (!cheats->DoesCheatCodeMatch(line, id)) return Error("Unknown cheat: " + text);
		const bool sent = cheats->SendCheatNotifications(line, id);
		McpLog::Write("run_cheat: %s (0x%08X) %s", text.c_str(), id, sent ? "sent" : "not sent");
		return json{ {"cheat", text}, {"id", Hex(id)}, {"sent", sent} };
	}

	json ReadLogTail(const std::string& fileName, size_t lines)
	{
		std::ifstream file(McpLog::DllFolder() / fileName);
		if (!file) return Error("Log file not found next to SC4MCP.dll.");
		std::vector<std::string> all;
		std::string line;
		while (std::getline(file, line)) all.push_back(line);
		const size_t start = all.size() > lines ? all.size() - lines : 0;
		json out = json::array();
		for (size_t i = start; i < all.size(); ++i) out.push_back(all[i]);
		return json{ {"file", fileName}, {"total_lines", all.size()}, {"lines", out} };
	}

	int MonthIndex(const json& date) { return date["year"].get<int>() * 12 + date["month"].get<int>(); }

	// Lets the simulation run until `months` months have passed (or `timeout_seconds`), then pauses it. Sends a
	// progress notification at every new month when the client asked for progress.
	json WaitMonths(int months, int speed, int timeoutSeconds, bool& isError, const Tools::Progress& progress)
	{
		json start = OnGameThread([&]() -> json
		{
			cISC4City* city = City();
			if (!city || !city->GetSimulator()) return NoCity();
			return DateOf(city->GetSimulator());
		}, isError);
		if (isError || !start.contains("year")) return start;
		bool speedError = false;
		SetSpeed(speed, speedError);

		const int startIndex = MonthIndex(start);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
		json now = start;
		int reported = 0;
		if (progress) progress(0, months, "Started at " + start["text"].get<std::string>());
		while (std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			bool pollError = false;
			json date = OnGameThread([&]() -> json
			{
				cISC4City* city = City();
				if (!city || !city->GetSimulator()) return NoCity();
				return DateOf(city->GetSimulator());
			}, pollError);
			if (pollError || !date.contains("year")) continue;
			now = date;
			const int passed = MonthIndex(now) - startIndex;
			if (progress && passed > reported)
			{
				reported = passed;
				progress(std::min(passed, months), months, "Reached " + now["text"].get<std::string>());
			}
			if (passed >= months) break;
		}
		bool stopError = false;
		SetSpeed(0, stopError);
		json status = OnGameThread(GetCityStatus, stopError);
		const bool reached = now.contains("year") && MonthIndex(now) - startIndex >= months;
		return json{ {"start_date", start}, {"months_requested", months}, {"months_passed", now.contains("year") ? MonthIndex(now) - startIndex : 0},
			{"reached", reached}, {"status", status} };
	}
}

namespace ToolsCommon
{
	void AddCoreTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "get_city_status",
			"Current city: name, size, date, pause state and speed, population, funds, monthly income/expense and the RCI demand of each type (city totals). get_demand gives the details.",
			Schema(json::object()), Permission::Read,
			GameThread([](const json&) { return GetCityStatus(); }) });

		tools.push_back({ "get_budget",
			"Budget of the open city: totals, loans, every department with its funding percentage and line items (expenses as the game holds them and as the sum of the current costs of the buildings). per_building=true adds every building with its cells and budget items.",
			Schema({ {"per_building", {{"type", "boolean"}, {"description", "Include every building with its budget items."}}} }), Permission::Read,
			GameThread([](const json& arguments) { return GetBudget(arguments.value("per_building", false)); }) });

		tools.push_back({ "list_buildings",
			"Buildings in the city with exemplar ID, city cells and monthly cost. Filter by name substring and/or occupant group (e.g. 0x1503 schools, 0x1507 health, 0x1400 power). Paged: limit and offset; next_offset is returned when more results remain.",
			Schema({ {"name_contains", {{"type", "string"}}}, {"occupant_group", {{"type", "string"}, {"description", "Hex occupant group, e.g. 0x1503."}}},
				{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}}, {"offset", {{"type", "integer"}, {"minimum", 0}, {"description", "Skip this many results (next_offset of the previous page)."}}} }), Permission::Read,
			GameThread([](const json& arguments)
			{
				const std::string filter = arguments.value("name_contains", std::string());
				const uint32_t group = arguments.contains("occupant_group") ? ParseId(arguments["occupant_group"]) : 0;
				const size_t limit = static_cast<size_t>(std::clamp(arguments.value("limit", 200), 1, 2000));
				const size_t offset = static_cast<size_t>(std::max(0, arguments.value("offset", 0)));
				return ListBuildings(filter, group, limit, offset);
			}) });

		tools.push_back({ "read_log",
			"Last lines of a log file next to the DLL: SC4SIM.log (default), SC4MCP.log or another *.log in the Plugins folder.",
			Schema({ {"file", {{"type", "string"}}}, {"lines", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}} }), Permission::Read,
			[](const json& arguments, bool& isError, const Tools::Context&) -> json
			{
				const std::string file = arguments.value("file", std::string("SC4SIM.log"));
				if (file.find_first_of("/\\:") != std::string::npos || file.size() < 5 || Lower(file.substr(file.size() - 4)) != ".log")
				{
					isError = true;
					return Error("file must be the name of a .log file in the Plugins folder.");
				}
				const size_t lines = static_cast<size_t>(std::clamp(arguments.value("lines", 100), 1, 1000));
				json out = ReadLogTail(file, lines);
				if (out.contains("error")) isError = true;
				return out;
			} });

		tools.push_back({ "set_speed",
			"Pause (0) or run the simulation at speed 1 (turtle) to 4 (cheetah). Returns the state after the game has applied it.",
			Schema({ {"speed", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}}} }, json::array({ "speed" })), Permission::Control,
			[](const json& arguments, bool& isError, const Tools::Context&) -> json
			{
				return SetSpeed(std::clamp(arguments.at("speed").get<int>(), 0, 4), isError);
			} });

		tools.push_back({ "wait_months",
			"Runs the simulation for the given number of game months (at speed 1-4), then pauses and returns the city status. Waits at most timeout_seconds (default 50) of real time. Sends progress notifications when the client asks for them.",
			Schema({ {"months", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}}}, {"speed", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4}}},
				{"timeout_seconds", {{"type", "integer"}, {"minimum", 5}, {"maximum", 600}}} }, json::array({ "months" })), Permission::Control,
			[](const json& arguments, bool& isError, const Tools::Context& context) -> json
			{
				const Tools::Progress& progress = context.progress;
				const int months = std::clamp(arguments.at("months").get<int>(), 1, 120);
				const int speed = std::clamp(arguments.value("speed", 4), 1, 4);
				const int timeout = std::clamp(arguments.value("timeout_seconds", 50), 5, 600);
				return WaitMonths(months, speed, timeout, isError, progress);
			} });

		tools.push_back({ "run_cheat",
			"Runs a cheat code as if typed in the Ctrl+X box, including the cheats of DLL plugins (e.g. FundingReset, ScanPumps, IMZoneConvert, RemovePhantomBudgetItems Health). Asks for confirmation; dry_run=true only checks the cheat.",
			Schema({ {"cheat", {{"type", "string"}}}, {"dry_run", {{"type", "boolean"}}}, {"confirm", {{"type", "boolean"}}} }, json::array({ "cheat" })), Permission::Cheats,
			Guarded([](const json& arguments) -> json
				{
					cISC4AppPtr app;
					cIGZCheatCodeManager* cheats = app ? app->GetCheatCodeManager() : nullptr;
					if (!cheats) return Error("Cheat code manager unavailable.");
					const std::string text = arguments.at("cheat").get<std::string>();
					cRZBaseString line(text.c_str());
					uint32_t id = 0;
					if (!cheats->DoesCheatCodeMatch(line, id)) return Error("Unknown cheat: " + text);
					return json{ {"action", "run_cheat"}, {"cheat", text}, {"id", Hex(id)} };
				},
				[](const json& arguments) { return RunCheat(arguments.at("cheat").get<std::string>()); },
				[](const json& plan) { return "Run the cheat \"" + plan.value("cheat", std::string()) + "\"?"; }) });
	}
}

// ===================================================================================================== Tools API

void Tools::Configure(bool allowControl, bool allowCheats, bool allowBuild, bool confirm)
{
	controlAllowed = allowControl;
	cheatsAllowed = allowCheats;
	buildAllowed = allowBuild;
	confirmChanges = confirm;
}

namespace ToolsCommon
{
	bool ConfirmChangesEnabled() { return confirmChanges; }

	json CallTool(const std::string& name, const json& arguments, bool& isError)
	{
		return Tools::Call(name, arguments, isError);
	}

	Handler Guarded(std::function<json(const json&)> preview, std::function<json(const json&)> apply,
		std::function<std::string(const json&)> question)
	{
		return [preview, apply, question](const json& arguments, bool& isError, const Tools::Context& context) -> json
		{
			json plan = OnGameThread([&] { return preview(arguments); }, isError);
			if (isError) return plan;
			if (arguments.value("dry_run", false))
			{
				plan["dry_run"] = true;
				return plan;
			}
			if (ConfirmChangesEnabled() && !arguments.value("confirm", false))
			{
				const int answer = context.confirm ? context.confirm(question(plan)) : -1;
				if (answer == 0)
				{
					plan["cancelled"] = true;
					plan["note"] = "The user did not confirm the change: nothing was changed.";
					return plan;
				}
				if (answer < 0)
				{
					plan["confirmation_required"] = true;
					plan["note"] = "Nothing was changed. Show this preview to the user and, if they agree, call the tool again with the same arguments and confirm=true.";
					return plan;
				}
			}
			json result = OnGameThread([&] { return apply(arguments); }, isError);
			return result;
		};
	}
}

json Tools::List()
{
	json list = json::array();
	for (const ToolDef& tool : Registry())
	{
		if (!Allowed(tool.permission)) continue;
		json entry{ {"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.inputSchema},
			{"outputSchema", json{ {"type", "object"}, {"description", "The result as a JSON object; on failure it has an \"error\" string."} }} };
		const bool readOnly = tool.permission == Permission::Read;
		entry["annotations"] = json{ {"readOnlyHint", readOnly}, {"destructiveHint", tool.permission == Permission::Build || tool.permission == Permission::Cheats},
			{"idempotentHint", readOnly}, {"openWorldHint", false} };
		list.push_back(entry);
	}
	return list;
}

json Tools::Call(const std::string& name, const json& arguments, bool& isError, const Context& context)
{
	isError = false;
	for (const ToolDef& tool : Registry())
	{
		if (tool.name != name) continue;
		if (!Allowed(tool.permission))
		{
			isError = true;
			return Error("Tool " + name + " is turned off in SC4MCP.ini.");
		}
		try
		{
			return tool.run(arguments, isError, context);
		}
		catch (const std::exception& error)
		{
			isError = true;
			return Error(std::string("Invalid arguments: ") + error.what());
		}
	}
	isError = true;
	return Error("Unknown tool: " + name);
}

// ------------------------------------------------------------------------------------------------- resources

namespace
{
	struct ResourceDef { const char* uri; const char* name; const char* description; const char* tool; json arguments; };

	const std::vector<ResourceDef>& Resources()
	{
		static const std::vector<ResourceDef> resources = {
			{ "sc4://city/status", "City status", "Name, date, speed, population, funds and RCI demand of the open city.", "get_city_status", json::object() },
			{ "sc4://city/budget", "Budget", "Departments, line items and totals of the city budget.", "get_budget", json::object() },
			{ "sc4://city/demand", "Demand", "RCI demand of every type, with supply and caps.", "get_demand", json::object() },
			{ "sc4://city/utilities", "Utilities", "Power, water and garbage: production, consumption and plants.", "get_utilities", json::object() },
			{ "sc4://city/services", "Services", "Education, health, police, jails and fire coverage and capacity.", "get_services", json::object() },
			{ "sc4://city/ordinances", "Ordinances", "Every ordinance with its state and monthly income or cost.", "list_ordinances", json::object() },
			{ "sc4://city/transport", "Transport", "Commuting, transit, congestion and potholes.", "get_transport", json::object() },
			{ "sc4://city/loans", "Loans", "Loans, borrowing limit and loan costs.", "get_loans", json::object() },
			{ "sc4://city/neighbors", "Neighbour deals", "Deals with the neighbouring cities and their income and expense.", "get_neighbor_deals", json::object() },
			{ "sc4://city/growth", "Growth diagnosis", "Why the city does or does not grow.", "diagnose_growth", json::object() },
			{ "sc4://log/SC4SIM.log", "SC4SIM.log", "The last 200 lines of SC4SIM.log.", "read_log", json{ {"file", "SC4SIM.log"}, {"lines", 200} } },
			{ "sc4://log/SC4MCP.log", "SC4MCP.log", "The last 200 lines of SC4MCP.log.", "read_log", json{ {"file", "SC4MCP.log"}, {"lines", 200} } },
		};
		return resources;
	}
}

json Tools::ListResources()
{
	json list = json::array();
	for (const ResourceDef& resource : Resources())
	{
		list.push_back(json{ {"uri", resource.uri}, {"name", resource.name}, {"description", resource.description}, {"mimeType", "application/json"} });
	}
	return list;
}

json Tools::ReadResource(const std::string& uri)
{
	for (const ResourceDef& resource : Resources())
	{
		if (uri != resource.uri) continue;
		bool isError = false;
		json data = Call(resource.tool, resource.arguments, isError);
		return json::array({ json{ {"uri", uri}, {"mimeType", "application/json"}, {"text", data.dump(2)} } });
	}
	return nullptr;
}

// ------------------------------------------------------------------------------------------------- prompts

json Tools::ListPrompts()
{
	return json::array({
		json{ {"name", "balance_test"}, {"title", "Balance test"},
			{"description", "Run the city for some months and compare the budget, services and utilities before and after."},
			{"arguments", json::array({
				json{ {"name", "months"}, {"description", "Game months to run (default 12)."}, {"required", false} },
				json{ {"name", "focus"}, {"description", "What to check: budget, services, utilities, demand or all (default all)."}, {"required", false} } })} },
		json{ {"name", "city_report"}, {"title", "City report"},
			{"description", "A short report on the open city: finances, demand, services, utilities and problems."},
			{"arguments", json::array()} },
		json{ {"name", "find_problems"}, {"title", "Find problems"},
			{"description", "Look for the city's weak points: deficits, unpowered or unwatered areas, low coverage, abandoned buildings, high crime or pollution."},
			{"arguments", json::array()} } });
}

json Tools::GetPrompt(const std::string& name, const json& arguments)
{
	auto message = [](const std::string& text)
	{
		return json::array({ json{ {"role", "user"}, {"content", json{ {"type", "text"}, {"text", text}}} } });
	};
	auto argument = [&](const char* key, const std::string& fallback)
	{
		return arguments.contains(key) && arguments[key].is_string() && !arguments[key].get<std::string>().empty() ? arguments[key].get<std::string>() : fallback;
	};
	if (name == "balance_test")
	{
		const std::string months = argument("months", "12");
		const std::string focus = argument("focus", "all");
		return json{ {"description", "Balance test over " + months + " months"}, {"messages", message(
			"Run a balance test of the open SimCity 4 city with the SC4MCP tools.\n"
			"1. Call get_city_status, get_budget, get_demand, get_services and get_utilities and keep the results as the baseline.\n"
			"2. Call wait_months with months=" + months + " (raise timeout_seconds if needed; it pauses the game at the end).\n"
			"3. Call the same tools again, and get_history for the graphs that matter (list_graphs gives them).\n"
			"4. Compare before and after with a focus on: " + focus + ". Report the numbers that changed, what looks unbalanced "
			"(costs against capacity, coverage, deficits, demand caps) and possible causes. Read SC4SIM.log with read_log if a value looks wrong.\n"
			"Do not change taxes, funding, ordinances or zones unless asked.")} };
	}
	if (name == "city_report")
	{
		return json{ {"description", "Report on the open city"}, {"messages", message(
			"Write a short report on the open SimCity 4 city using the SC4MCP tools: get_city_status, get_budget, get_demand, "
			"get_services, get_utilities, list_ordinances and get_history (population and funds over the last 24 months). "
			"Cover finances, demand, services, utilities, and the three most important problems with a suggestion for each. "
			"Do not change anything in the city.")} };
	}
	if (name == "find_problems")
	{
		return json{ {"description", "Weak points of the open city"}, {"messages", message(
			"Find the weak points of the open SimCity 4 city with the SC4MCP tools. Check: the monthly balance (get_budget), "
			"power and water shortfalls (get_utilities), education, health, police and fire coverage and capacity (get_services), "
			"abandoned or unpowered buildings (list_lots with state=abandoned or powered=false), and the worst areas for crime, "
			"pollution and land value (get_map_stats, and render_map to look at them). Start with diagnose_growth. For each problem give where it is (cells) and a concrete fix. "
			"Do not change anything in the city.")} };
	}
	return nullptr;
}
