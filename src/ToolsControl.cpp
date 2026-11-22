// SC4MCP - tools that change the city: department and building funding, taxes, ordinances, saving, and zoning
// (which, like the game's zone tools, can demolish the lots it covers when that is allowed).
//
// Control tools need AllowControl=1, zoning needs AllowBuild=1 (SC4MCP.ini). Every change is written to SC4MCP.log.
#include "ToolsCommon.h"
#include "McpLog.h"

#include "cIGZString.h"
#include "cISC4App.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4City.h"
#include "cISC4DepartmentBudget.h"
#include "cISC4Lot.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISC4Ordinance.h"
#include "cISC4OrdinanceSimulator.h"
#include "cISC4ZoneManager.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"
#include "SC4CellRegion.h"
#include "SC4Percentage.h"

#include <algorithm>
#include <cmath>

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	// ------------------------------------------------------------------------------------------------- funding

	json SetDepartmentFunding(uint32_t departmentID, double percent)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		cISC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(departmentID) : nullptr;
		if (!department) return Error("No department " + Hex(departmentID) + " (get_budget lists the departments).");
		if (department->GetIsFixedFunding()) return Error("The funding of this department is fixed by the game.");
		const SC4Percentage* maximum = department->GetMaxAllowedFundingPercentage();
		const double maxPercent = maximum ? maximum->percentage * 100.0 : 120.0;
		const double clamped = std::clamp(percent, 0.0, maxPercent);
		const SC4Percentage* before = department->GetFundingPercentage();
		const double beforePercent = before ? before->percentage * 100.0 : 0.0;
		SC4Percentage value{};
		value.percentage = static_cast<float>(clamped / 100.0);
		department->SetFundingPercentage(value, 0);
		const SC4Percentage* after = department->GetFundingPercentage();
		const double afterPercent = after ? after->percentage * 100.0 : clamped;
		McpLog::Write("set_funding: department 0x%08X %.1f%% -> %.1f%%", departmentID, beforePercent, afterPercent);
		return json{ {"department", Hex(departmentID)}, {"name", DepartmentName(department)}, {"before_percent", beforePercent},
			{"funding_percent", afterPercent}, {"max_percent", maxPercent}, {"clamped", clamped != percent} };
	}

	json SetBuildingFunding(int32_t x, int32_t z, double percent, uint32_t purpose)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		if (!budget) return Error("Budget simulator unavailable.");
		cISC4Occupant* building = BuildingAt(city, x, z);
		if (!building) return Error("No building on that cell.");
		const BuildingCost cost = CostOf(budget, building);
		if (cost.items.empty()) return Error("This building has no budget items.");
		cISCPropertyHolder* holder = building->AsPropertyHolder();
		const double clamped = std::clamp(percent, 0.0, 120.0);
		SC4Percentage value{};
		value.percentage = static_cast<float>(clamped / 100.0);
		json changed = json::array();
		for (const auto& item : cost.items)
		{
			if (purpose != 0 && item.purpose != purpose) continue;
			const bool ok = budget->ChangeBudgetItemLocalFunding(holder, item.purpose, value);
			changed.push_back(json{ {"purpose", Hex(item.purpose)}, {"department", Hex(item.department)}, {"before_percent", item.localFunding * 100.0f}, {"changed", ok} });
		}
		if (changed.empty()) return Error("The building has no budget item with that purpose.");
		McpLog::Write("set_building_funding: %s at (%d,%d) -> %.1f%%", BuildingName(building).c_str(), x, z, clamped);
		json after = json::array();
		for (const auto& item : CostOf(budget, building).items)
		{
			after.push_back(json{ {"purpose", Hex(item.purpose)}, {"local_funding_percent", item.localFunding * 100.0f}, {"cost", item.cost} });
		}
		return json{ {"building", BuildingName(building)}, {"cells", BuildingCells(building)}, {"funding_percent", clamped}, {"changes", changed}, {"items_after", after} };
	}

	// ------------------------------------------------------------------------------------------------- taxes

	struct TaxType { const char* name; const char* luaKey; };
	constexpr TaxType kTaxes[] = {
		{"r_low", "g_tax_rate_r_low"}, {"r_medium", "g_tax_rate_r_med"}, {"r_high", "g_tax_rate_r_high"},
		{"cs_low", "g_tax_rate_cs_low"}, {"cs_medium", "g_tax_rate_cs_med"}, {"cs_high", "g_tax_rate_cs_high"},
		{"co_medium", "g_tax_rate_co_med"}, {"co_high", "g_tax_rate_co_high"},
		{"i_agriculture", "g_tax_rate_i_resource"}, {"i_dirty", "g_tax_rate_i_dirty"},
		{"i_manufacturing", "g_tax_rate_i_manufacturing"}, {"i_hightech", "g_tax_rate_i_hightech"} };

	// The budget simulator's tax group IDs are not documented. Candidate numberings are tried, and the one whose
	// GetTaxRate values match the twelve rates of the advisor data is used, so a wrong guess never writes a rate.
	bool FindTaxGroups(cISC4BudgetSimulator* budget, const GameTable& table, uint32_t (&groups)[12])
	{
		static const uint32_t kCandidates[][12] = {
			{ 0x1010, 0x1020, 0x1030, 0x3110, 0x3120, 0x3130, 0x3320, 0x3330, 0x4100, 0x4200, 0x4300, 0x4400 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 },
			{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 } };
		for (const auto& candidate : kCandidates)
		{
			int matched = 0, nonZero = 0;
			for (int i = 0; i < 12; ++i)
			{
				const auto found = table.numbers.find(kTaxes[i].luaKey);
				if (found == table.numbers.end()) return false;
				const double rate = budget->GetTaxRate(candidate[i]);
				if (std::fabs(rate - found->second) < 0.051) ++matched;
				if (found->second > 0.0) ++nonZero;
			}
			if (matched == 12 && nonZero > 0)
			{
				std::copy(std::begin(candidate), std::end(candidate), std::begin(groups));
				return true;
			}
		}
		return false;
	}

	json GetTaxes()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		GameTable table;
		if (!ReadGameTable(city, table)) return Error("The advisor data table is not available yet (let one month pass).");
		json rates = json::object();
		for (const TaxType& tax : kTaxes)
		{
			const auto found = table.numbers.find(tax.luaKey);
			if (found != table.numbers.end()) rates[tax.name] = found->second;
		}
		json out{ {"rates_percent", rates} };
		if (cISC4BudgetSimulator* budget = city->GetBudgetSimulator()) out["tax_income_ytd"] = budget->GetTaxIncome();
		return out;
	}

	json SetTax(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		if (!budget) return Error("Budget simulator unavailable.");
		GameTable table;
		if (!ReadGameTable(city, table)) return Error("The advisor data table is not available yet (let one month pass).");
		uint32_t groups[12]{};
		if (!FindTaxGroups(budget, table, groups))
		{
			return Error("Could not identify the game's tax groups (the budget rates did not match the advisor data); no rate was changed.");
		}

		// type: one name, "all", "residential", "commercial" or "industrial".
		const std::string type = Lower(arguments.at("type").get<std::string>());
		const double rate = std::round(std::clamp(arguments.at("rate").get<double>(), 0.0, 20.0) * 10.0) / 10.0;
		json changed = json::array();
		for (int i = 0; i < 12; ++i)
		{
			const std::string name = kTaxes[i].name;
			const bool match = type == "all" || type == name
				|| (type == "residential" && name[0] == 'r') || (type == "commercial" && name[0] == 'c') || (type == "industrial" && name[0] == 'i');
			if (!match) continue;
			const double before = budget->GetTaxRate(groups[i]);
			const bool ok = budget->SetTaxRate(groups[i], static_cast<float>(rate));
			changed.push_back(json{ {"type", name}, {"before", before}, {"after", budget->GetTaxRate(groups[i])}, {"ok", ok} });
		}
		if (changed.empty()) return Error("Unknown tax type. Use one of r_low, r_medium, r_high, cs_low, cs_medium, cs_high, co_medium, co_high, i_agriculture, i_dirty, i_manufacturing, i_hightech, residential, commercial, industrial or all.");
		McpLog::Write("set_tax: %s -> %.1f%%", type.c_str(), rate);
		return json{ {"rate", rate}, {"changes", changed}, {"note", "The advisor data shows the new rates after the next monthly update."} };
	}

	// ------------------------------------------------------------------------------------------------- ordinances

	json SetOrdinance(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4OrdinanceSimulator* ordinances = city->GetOrdinanceSimulator();
		if (!ordinances) return Error("Ordinance simulator unavailable.");
		const bool on = arguments.at("on").get<bool>();

		uint32_t id = 0;
		const json& which = arguments.at("ordinance");
		try { id = ParseId(which); }
		catch (const std::exception&)
		{
			// A name: the first ordinance whose name contains the text.
			const std::string needle = Lower(which.get<std::string>());
			uint32_t ids[512]{};
			uint32_t count = 512;
			ordinances->GetOrdinanceIDArray(ids, count);
			for (uint32_t i = 0; i < std::min<uint32_t>(count, 512) && id == 0; ++i)
			{
				cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(ids[i]);
				cIGZString* name = ordinance ? ordinance->GetName() : nullptr;
				if (name && Lower(name->ToChar()).find(needle) != std::string::npos) id = ids[i];
			}
		}
		cISC4Ordinance* ordinance = id ? ordinances->GetOrdinanceByID(id) : nullptr;
		if (!ordinance) return Error("No such ordinance (list_ordinances lists them).");
		cIGZString* name = ordinance->GetName();
		const std::string label = name ? name->ToChar() : Hex(id);
		if (on && !ordinances->IsOrdinanceAvailable(id)) return Error(label + " is not available yet in this city.");
		const bool before = ordinances->IsOrdinanceOn(id);
		const bool ok = ordinances->SetOrdinanceOn(id, on);
		const bool after = ordinances->IsOrdinanceOn(id);
		McpLog::Write("set_ordinance: %s (0x%08X) %s -> %s", label.c_str(), id, before ? "on" : "off", after ? "on" : "off");
		return json{ {"id", Hex(id)}, {"name", label}, {"before", before}, {"on", after}, {"ok", ok},
			{"monthly_adjusted_income", ordinance->GetMonthlyAdjustedIncome()} };
	}

	// ------------------------------------------------------------------------------------------------- save

	json SaveCity()
	{
		cISC4AppPtr app;
		if (!app || !app->GetCity()) return NoCity();
		const bool requested = app->RequestSaveCity(false, false);
		McpLog::Write("save_city: %s", requested ? "requested" : "refused");
		return json{ {"requested", requested}, {"note", "The game saves the city on its next frames, as with Ctrl+S."} };
	}

	// ------------------------------------------------------------------------------------------------- zoning

	json PlaceZone(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4ZoneManager* zones = city->GetZoneManager();
		cISC4LotManager* lots = city->GetLotManager();
		if (!zones || !lots) return Error("Zone manager unavailable.");

		const int32_t zone = ParseZone(arguments.at("zone"));
		if (zone < 0 || zone > 9) return Error("zone must be none (dezone), residential_low/medium/high, commercial_low/medium/high, agriculture or industrial_medium/high.");
		int32_t x1 = arguments.at("x1").get<int32_t>(), z1 = arguments.at("z1").get<int32_t>();
		int32_t x2 = arguments.value("x2", x1), z2 = arguments.value("z2", z1);
		if (x1 > x2) std::swap(x1, x2);
		if (z1 > z2) std::swap(z1, z2);
		if (!InCity(city, x1, z1) || !InCity(city, x2, z2)) return Error("The rectangle is outside the city.");
		if ((x2 - x1 + 1) * (z2 - z1 + 1) > 64 * 64) return Error("At most 4096 cells (64 x 64) per call.");
		const bool allowDemolition = arguments.value("allow_demolition", false);

		// Lots inside the rectangle: the game demolishes a lot whose zone changes, so they are refused unless allowed.
		// Plopped buildings (civics, utilities, rewards) are never touched.
		json lotCells = json::array();
		for (int32_t x = x1; x <= x2; ++x)
		{
			for (int32_t z = z1; z <= z2; ++z)
			{
				cISC4Lot* lot = lots->GetLot(x, z, false);
				const int32_t current = static_cast<int32_t>(zones->GetZoneType(x, z));
				if (current >= 10) return Error("The rectangle contains a " + std::string(ZoneName(current)) + " cell at (" + std::to_string(x) + ", " + std::to_string(z) + "): plopped buildings, airports, seaports and landfills are not changed.");
				if (lot && lotCells.size() < 20) lotCells.push_back(json{ {"x", x}, {"z", z} });
				else if (lot) lotCells.push_back(nullptr);
			}
		}
		if (!lotCells.empty() && !allowDemolition)
		{
			return json{ {"error", "The rectangle contains developed lots; changing their zone demolishes them. Pass allow_demolition=true to do it anyway."},
				{"lot_cells", lotCells.size()} };
		}

		SC4CellRegion<int32_t> region(x1, z1, x2, z2, true);
		int64_t zoned = 0;
		int32_t errorCode = 0;
		// Same flags as the game's own call that dezones a rectangle (0x00467075): place, skip the funds check,
		// no slope check, skip the zone size check, check the lot size.
		const bool ok = zones->PlaceZone(region, static_cast<cISC4ZoneManager::ZoneType>(zone), true, true, false, true, true, &zoned, &errorCode, 0);
		McpLog::Write("place_zone: %s (%d,%d)-(%d,%d) %s, %lld cells, error %d", ZoneName(zone), x1, z1, x2, z2, ok ? "done" : "refused",
			static_cast<long long>(zoned), errorCode);
		json out{ {"zone", ZoneName(zone)}, {"rect", json{ {"x1", x1}, {"z1", z1}, {"x2", x2}, {"z2", z2} }}, {"ok", ok},
			{"cells_changed", zoned}, {"error_code", errorCode}, {"lots_in_rect", lotCells.size()} };
		if (!ok) out["error"] = "The game refused the zone (water, slope, network or a lot that cannot be rezoned).";
		return out;
	}
}

namespace ToolsCommon
{
	void AddControlTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "get_taxes",
			"The twelve tax rates (percent) of the city, as the budget window shows them.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetTaxes(); }) });

		tools.push_back({ "set_funding",
			"Sets the funding slider of a budget department (department ID from get_budget) to a percentage (0 up to the department maximum, usually 120).",
			Schema({ {"department", {{"type", "string"}, {"description", "Department ID, e.g. \"0xEA54D286\"."}}}, {"percent", {{"type", "number"}, {"minimum", 0}, {"maximum", 200}}} },
				json::array({ "department", "percent" })), Permission::Control,
			GameThread([](const json& arguments) { return SetDepartmentFunding(ParseId(arguments.at("department")), arguments.at("percent").get<double>()); }) });

		tools.push_back({ "set_building_funding",
			"Sets the local funding of one building (any cell of its lot) to a percentage, for all its budget items or for one purpose.",
			Schema({ {"x", {{"type", "integer"}}}, {"z", {{"type", "integer"}}}, {"percent", {{"type", "number"}, {"minimum", 0}, {"maximum", 120}}},
				{"purpose", {{"type", "string"}, {"description", "Optional purpose ID from get_budget per_building."}}} },
				json::array({ "x", "z", "percent" })), Permission::Control,
			GameThread([](const json& arguments)
			{
				const uint32_t purpose = arguments.contains("purpose") ? ParseId(arguments["purpose"]) : 0;
				return SetBuildingFunding(arguments.at("x").get<int32_t>(), arguments.at("z").get<int32_t>(), arguments.at("percent").get<double>(), purpose);
			}) });

		tools.push_back({ "set_tax",
			"Sets a tax rate (0-20 %, 0.1 steps): type is one of r_low, r_medium, r_high, cs_low, cs_medium, cs_high, co_medium, co_high, i_agriculture, i_dirty, i_manufacturing, i_hightech, or residential, commercial, industrial, all.",
			Schema({ {"type", {{"type", "string"}}}, {"rate", {{"type", "number"}, {"minimum", 0}, {"maximum", 20}}} }, json::array({ "type", "rate" })), Permission::Control,
			GameThread([](const json& arguments) { return SetTax(arguments); }) });

		tools.push_back({ "set_ordinance",
			"Turns an ordinance on or off: ordinance is its ID or part of its name (list_ordinances lists them).",
			Schema({ {"ordinance", {{"type", "string"}}}, {"on", {{"type", "boolean"}}} }, json::array({ "ordinance", "on" })), Permission::Control,
			GameThread([](const json& arguments) { return SetOrdinance(arguments); }) });

		tools.push_back({ "save_city",
			"Saves the open city, as Ctrl+S does.",
			Schema(json::object()), Permission::Control, GameThread([](const json&) { return SaveCity(); }) });

		tools.push_back({ "place_zone",
			"Zones (or dezones, zone=none) a rectangle of cells, like the game's zone tools: zone is residential_low/medium/high, commercial_low/medium/high, agriculture, industrial_medium/high or none. Developed lots in the rectangle are refused unless allow_demolition=true, because changing their zone demolishes them; plopped buildings are never touched. At most 64 x 64 cells.",
			Schema({ {"zone", {{"type", "string"}}}, {"x1", {{"type", "integer"}}}, {"z1", {{"type", "integer"}}}, {"x2", {{"type", "integer"}}}, {"z2", {{"type", "integer"}}},
				{"allow_demolition", {{"type", "boolean"}}} }, json::array({ "zone", "x1", "z1" })), Permission::Build,
			GameThread([](const json& arguments) { return PlaceZone(arguments); }) });
	}
}
