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
#include "cISC4DemandSimulator.h"
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
#include <cstdio>
#include <set>
#include <map>

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	// ------------------------------------------------------------------------------------------------- funding

	json PreviewDepartmentFunding(uint32_t departmentID, double percent)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		cISC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(departmentID) : nullptr;
		if (!department) return Error("No department " + Hex(departmentID) + " (get_budget lists the departments).");
		if (department->GetIsFixedFunding()) return Error("The funding of this department is fixed by the game.");
		const SC4Percentage* maximum = department->GetMaxAllowedFundingPercentage();
		const double maxPercent = maximum ? maximum->percentage * 100.0 : 120.0;
		const SC4Percentage* before = department->GetFundingPercentage();
		return json{ {"action", "set_funding"}, {"department", Hex(departmentID)}, {"name", DepartmentName(department)},
			{"current_percent", before ? before->percentage * 100.0 : 0.0}, {"new_percent", std::clamp(percent, 0.0, maxPercent)},
			{"max_percent", maxPercent}, {"monthly_expenses", department->GetTotalExpenses()} };
	}

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
		RecordChange("Funding of " + DepartmentName(department) + " (" + Hex(departmentID) + ") " + std::to_string(static_cast<int>(beforePercent + 0.5)) + "% -> " + std::to_string(static_cast<int>(afterPercent + 0.5)) + "%",
			json::array({ json{ {"tool", "set_funding"}, {"arguments", json{ {"department", Hex(departmentID)}, {"percent", beforePercent} }} } }));
		return json{ {"department", Hex(departmentID)}, {"name", DepartmentName(department)}, {"before_percent", beforePercent},
			{"funding_percent", afterPercent}, {"max_percent", maxPercent}, {"clamped", clamped != percent} };
	}

	json PreviewBuildingFunding(int32_t x, int32_t z, double percent, uint32_t purpose)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		cISC4Occupant* building = budget ? BuildingAt(city, x, z) : nullptr;
		if (!building) return Error("No building on that cell.");
		const BuildingCost cost = CostOf(budget, building);
		json items = json::array();
		for (const auto& item : cost.items)
		{
			if (purpose != 0 && item.purpose != purpose) continue;
			items.push_back(json{ {"purpose", Hex(item.purpose)}, {"department", Hex(item.department)}, {"current_percent", item.localFunding * 100.0f}, {"cost", item.cost} });
		}
		if (items.empty()) return Error(cost.items.empty() ? "This building has no budget items." : "The building has no budget item with that purpose.");
		return json{ {"action", "set_building_funding"}, {"building", BuildingName(building)}, {"cells", BuildingCells(building)},
			{"new_percent", std::clamp(percent, 0.0, 120.0)}, {"items", items} };
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
		json undo = json::array();
		for (const json& change : changed)
		{
			if (!change.value("changed", false)) continue;
			undo.push_back(json{ {"tool", "set_building_funding"}, {"arguments", json{ {"x", x}, {"z", z}, {"purpose", change["purpose"]}, {"percent", change["before_percent"]} }} });
		}
		RecordChange("Local funding of " + BuildingName(building) + " at (" + std::to_string(x) + ", " + std::to_string(z) + ") -> " + std::to_string(static_cast<int>(clamped + 0.5)) + "%", undo);
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

	bool TaxMatches(const std::string& type, const std::string& name)
	{
		return type == "all" || type == name || (type == "residential" && name[0] == 'r') || (type == "commercial" && name[0] == 'c')
			|| (type == "industrial" && name[0] == 'i');
	}

	json PreviewTax(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		GameTable table;
		if (!budget || !ReadGameTable(city, table)) return Error("The advisor data table is not available yet (let one month pass).");
		uint32_t groups[12]{};
		if (!FindTaxGroups(budget, table, groups)) return Error("Could not identify the game's tax groups (the budget rates did not match the advisor data); no rate can be changed.");
		const std::string type = Lower(arguments.at("type").get<std::string>());
		const double rate = std::round(std::clamp(arguments.at("rate").get<double>(), 0.0, 20.0) * 10.0) / 10.0;
		json changes = json::array();
		for (int i = 0; i < 12; ++i)
		{
			if (TaxMatches(type, kTaxes[i].name)) changes.push_back(json{ {"type", kTaxes[i].name}, {"current", budget->GetTaxRate(groups[i])}, {"new", rate} });
		}
		if (changes.empty()) return Error("Unknown tax type. Use one of r_low, r_medium, r_high, cs_low, cs_medium, cs_high, co_medium, co_high, i_agriculture, i_dirty, i_manufacturing, i_hightech, residential, commercial, industrial or all.");
		json out{ {"action", "set_tax"}, {"rate", rate}, {"changes", changes} };
		if (cISC4DemandSimulator* demand = city->GetDemandSimulator()) out["neutral_tax_rate"] = demand->GetNeutralTaxRate();
		return out;
	}

	// Estimates the monthly tax income at other rates: income of a tax type ~ its weighted assessed value x its rate,
	// scaled so that the current rates give the current monthly tax income of the advisor data.
	json TaxWhatIf(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		GameTable table;
		if (!budget || !ReadGameTable(city, table)) return Error("The advisor data table is not available yet (let one month pass).");
		uint32_t groups[12]{};
		if (!FindTaxGroups(budget, table, groups)) return Error("Could not identify the game's tax groups.");

		std::map<std::string, double> newRates;
		if (arguments.contains("rates") && arguments["rates"].is_object())
		{
			for (auto it = arguments["rates"].begin(); it != arguments["rates"].end(); ++it) if (it.value().is_number()) newRates[Lower(it.key())] = it.value().get<double>();
		}
		if (arguments.contains("type") && arguments.contains("rate")) newRates[Lower(arguments["type"].get<std::string>())] = arguments["rate"].get<double>();
		if (newRates.empty()) return Error("Give type and rate, or rates: {\"r_low\": 8, \"industrial\": 10, ...}.");

		double weighted = 0.0;
		double current[12]{}, assessed[12]{}, proposed[12]{};
		for (int i = 0; i < 12; ++i)
		{
			current[i] = budget->GetTaxRate(groups[i]);
			assessed[i] = static_cast<double>(budget->GetWeightedAssessedTaxValue(groups[i]));
			proposed[i] = current[i];
			for (const auto& [type, rate] : newRates) if (TaxMatches(type, kTaxes[i].name)) proposed[i] = std::round(std::clamp(rate, 0.0, 20.0) * 10.0) / 10.0;
			weighted += assessed[i] * current[i];
		}
		const auto income = table.numbers.find("g_tax_income");
		const double monthlyTax = income != table.numbers.end() ? income->second : 0.0;
		const double k = weighted > 0.0 && monthlyTax > 0.0 ? monthlyTax / weighted : 0.0;
		cISC4DemandSimulator* demand = city->GetDemandSimulator();
		const double neutral = demand ? demand->GetNeutralTaxRate() : 0.0;
		json types = json::array();
		double totalNow = 0.0, totalNew = 0.0;
		for (int i = 0; i < 12; ++i)
		{
			const double now = assessed[i] * current[i] * k, then = assessed[i] * proposed[i] * k;
			totalNow += now;
			totalNew += then;
			if (proposed[i] == current[i]) continue;
			json t{ {"type", kTaxes[i].name}, {"current_rate", current[i]}, {"new_rate", proposed[i]}, {"assessed_value", assessed[i]},
				{"monthly_income_now", std::round(now)}, {"monthly_income_new", std::round(then)}, {"change", std::round(then - now)} };
			if (proposed[i] > neutral + 0.05) t["demand_effect"] = "above the neutral rate: lowers the demand of this type";
			else if (proposed[i] < current[i]) t["demand_effect"] = "lower rate: raises the demand of this type";
			types.push_back(t);
		}
		json out{ {"changes", types}, {"monthly_tax_income_now", std::round(totalNow)}, {"monthly_tax_income_new", std::round(totalNew)},
			{"monthly_change", std::round(totalNew - totalNow)}, {"neutral_tax_rate", neutral} };
		out["note"] = k > 0.0
			? "Estimate at the current population: income per type = assessed value x rate, scaled to the current monthly tax income. It ignores how the demand, and with it the population, will react."
			: "No current tax income in the advisor data: only the relative change (assessed value x rate) is meaningful.";
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
			if (!TaxMatches(type, name)) continue;
			const double before = budget->GetTaxRate(groups[i]);
			const bool ok = budget->SetTaxRate(groups[i], static_cast<float>(rate));
			changed.push_back(json{ {"type", name}, {"before", before}, {"after", budget->GetTaxRate(groups[i])}, {"ok", ok} });
		}
		if (changed.empty()) return Error("Unknown tax type. Use one of r_low, r_medium, r_high, cs_low, cs_medium, cs_high, co_medium, co_high, i_agriculture, i_dirty, i_manufacturing, i_hightech, residential, commercial, industrial or all.");
		McpLog::Write("set_tax: %s -> %.1f%%", type.c_str(), rate);
		json undo = json::array();
		for (const json& change : changed)
		{
			if (change.value("ok", false)) undo.push_back(json{ {"tool", "set_tax"}, {"arguments", json{ {"type", change["type"]}, {"rate", change["before"]} }} });
		}
		char rateText[16];
		std::snprintf(rateText, sizeof(rateText), "%.1f", rate);
		RecordChange("Tax " + type + " -> " + rateText + "%", undo);
		return json{ {"rate", rate}, {"changes", changed}, {"note", "The advisor data shows the new rates after the next monthly update."} };
	}

	// ------------------------------------------------------------------------------------------------- ordinances

	uint32_t FindOrdinance(cISC4OrdinanceSimulator* ordinances, const json& which);

	json PreviewOrdinance(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4OrdinanceSimulator* ordinances = city->GetOrdinanceSimulator();
		if (!ordinances) return Error("Ordinance simulator unavailable.");
		const uint32_t id = FindOrdinance(ordinances, arguments.at("ordinance"));
		cISC4Ordinance* ordinance = id ? ordinances->GetOrdinanceByID(id) : nullptr;
		if (!ordinance) return Error("No such ordinance (list_ordinances lists them).");
		cIGZString* name = ordinance->GetName();
		const bool on = arguments.at("on").get<bool>();
		if (on && !ordinances->IsOrdinanceAvailable(id)) return Error(std::string(name ? name->ToChar() : Hex(id).c_str()) + " is not available yet in this city.");
		return json{ {"action", "set_ordinance"}, {"id", Hex(id)}, {"name", name ? name->ToChar() : ""}, {"currently_on", ordinances->IsOrdinanceOn(id)},
			{"turn_on", on}, {"monthly_adjusted_income", ordinance->GetMonthlyAdjustedIncome()}, {"enactment_income", ordinance->GetEnactmentIncome()} };
	}

	uint32_t FindOrdinance(cISC4OrdinanceSimulator* ordinances, const json& which)
	{
		uint32_t id = 0;
		try { id = ParseId(which); }
		catch (const std::exception&)
		{
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
		return id;
	}

	json SetOrdinance(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4OrdinanceSimulator* ordinances = city->GetOrdinanceSimulator();
		if (!ordinances) return Error("Ordinance simulator unavailable.");
		const bool on = arguments.at("on").get<bool>();

		const uint32_t id = FindOrdinance(ordinances, arguments.at("ordinance"));
		cISC4Ordinance* ordinance = id ? ordinances->GetOrdinanceByID(id) : nullptr;
		if (!ordinance) return Error("No such ordinance (list_ordinances lists them).");
		cIGZString* name = ordinance->GetName();
		const std::string label = name ? name->ToChar() : Hex(id);
		if (on && !ordinances->IsOrdinanceAvailable(id)) return Error(label + " is not available yet in this city.");
		const bool before = ordinances->IsOrdinanceOn(id);
		const bool ok = ordinances->SetOrdinanceOn(id, on);
		const bool after = ordinances->IsOrdinanceOn(id);
		McpLog::Write("set_ordinance: %s (0x%08X) %s -> %s", label.c_str(), id, before ? "on" : "off", after ? "on" : "off");
		if (before != after)
		{
			RecordChange(std::string(after ? "Enacted " : "Repealed ") + label,
				json::array({ json{ {"tool", "set_ordinance"}, {"arguments", json{ {"ordinance", Hex(id)}, {"on", before} }} } }));
		}
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

	// Checks a zoning request; on success fills the rectangle and returns the preview.
	json PlanZone(const json& arguments, int32_t& zone, int32_t& x1, int32_t& z1, int32_t& x2, int32_t& z2)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4ZoneManager* zones = city->GetZoneManager();
		cISC4LotManager* lots = city->GetLotManager();
		if (!zones || !lots) return Error("Zone manager unavailable.");
		zone = ParseZone(arguments.at("zone"));
		if (zone < 0 || zone > 9) return Error("zone must be none (dezone), residential_low/medium/high, commercial_low/medium/high, agriculture or industrial_medium/high.");
		x1 = arguments.at("x1").get<int32_t>(); z1 = arguments.at("z1").get<int32_t>();
		x2 = arguments.value("x2", x1); z2 = arguments.value("z2", z1);
		if (x1 > x2) std::swap(x1, x2);
		if (z1 > z2) std::swap(z1, z2);
		if (!InCity(city, x1, z1) || !InCity(city, x2, z2)) return Error("The rectangle is outside the city.");
		if ((x2 - x1 + 1) * (z2 - z1 + 1) > 64 * 64) return Error("At most 4096 cells (64 x 64) per call.");
		std::set<cISC4Lot*> lotSet;
		json buildings = json::array();
		int changed = 0;
		for (int32_t x = x1; x <= x2; ++x)
		{
			for (int32_t z = z1; z <= z2; ++z)
			{
				const int32_t current = static_cast<int32_t>(zones->GetZoneType(x, z));
				if (current >= 10) return Error("The rectangle contains a " + std::string(ZoneName(current)) + " cell at (" + std::to_string(x) + ", " + std::to_string(z) + "): plopped buildings, airports, seaports and landfills are not changed.");
				if (current != zone) ++changed;
				cISC4Lot* lot = nullptr;
				cISC4Occupant* building = BuildingAt(city, x, z, &lot);
				if (lot && lotSet.insert(lot).second && buildings.size() < 30)
				{
					buildings.push_back(json{ {"x", x}, {"z", z}, {"building", building ? BuildingName(building) : std::string("(empty lot)")} });
				}
			}
		}
		return json{ {"action", "place_zone"}, {"zone", ZoneName(zone)}, {"rect", json{ {"x1", x1}, {"z1", z1}, {"x2", x2}, {"z2", z2} }},
			{"cells", (x2 - x1 + 1) * (z2 - z1 + 1)}, {"cells_with_another_zone", changed}, {"lots_demolished", lotSet.size()}, {"lots", buildings},
			{"allow_demolition", arguments.value("allow_demolition", false)} };
	}

	json PreviewZone(const json& arguments)
	{
		int32_t zone, x1, z1, x2, z2;
		json plan = PlanZone(arguments, zone, x1, z1, x2, z2);
		if (!plan.contains("error") && plan["lots_demolished"].get<size_t>() > 0 && !arguments.value("allow_demolition", false))
		{
			plan["error"] = "The rectangle contains developed lots; changing their zone demolishes them. Pass allow_demolition=true to do it anyway.";
		}
		return plan;
	}

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
	namespace
	{
		// dry_run and confirm, added to the schema of every tool that changes the city.
		json WithConfirm(json properties)
		{
			properties["dry_run"] = json{ {"type", "boolean"}, {"description", "Only describe the change, do not apply it."} };
			properties["confirm"] = json{ {"type", "boolean"}, {"description", "Apply without asking (the user has already agreed)."} };
			return properties;
		}

		std::string Number(const json& value)
		{
			char text[32];
			std::snprintf(text, sizeof(text), "%.1f", value.is_number() ? value.get<double>() : 0.0);
			return text;
		}
	}

	void AddControlTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "get_taxes",
			"The twelve tax rates (percent) of the city, as the budget window shows them.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetTaxes(); }) });

		tools.push_back({ "tax_what_if",
			"Estimates the monthly tax income with other tax rates, without changing anything: give type and rate, or rates as an object (e.g. {\"r_low\": 8, \"industrial\": 10}); types as in set_tax. Also says which rates would be above the neutral rate (lower demand).",
			Schema({ {"type", {{"type", "string"}}}, {"rate", {{"type", "number"}, {"minimum", 0}, {"maximum", 20}}}, {"rates", {{"type", "object"}}} }), Permission::Read,
			GameThread([](const json& arguments) { return TaxWhatIf(arguments); }) });

		tools.push_back({ "set_funding",
			"Sets the funding slider of a budget department (department ID from get_budget) to a percentage (0 up to the department maximum, usually 120). Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"department", {{"type", "string"}, {"description", "Department ID, e.g. \"0xEA54D286\"."}}}, {"percent", {{"type", "number"}, {"minimum", 0}, {"maximum", 200}}} }),
				json::array({ "department", "percent" })), Permission::Control,
			Guarded([](const json& a) { return PreviewDepartmentFunding(ParseId(a.at("department")), a.at("percent").get<double>()); },
				[](const json& a) { return SetDepartmentFunding(ParseId(a.at("department")), a.at("percent").get<double>()); },
				[](const json& p) { return "Set the funding of " + p.value("name", std::string("the department")) + " from " + Number(p["current_percent"]) + "% to " + Number(p["new_percent"]) + "%?"; }) });

		tools.push_back({ "set_building_funding",
			"Sets the local funding of one building (any cell of its lot) to a percentage, for all its budget items or for one purpose. Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"x", {{"type", "integer"}}}, {"z", {{"type", "integer"}}}, {"percent", {{"type", "number"}, {"minimum", 0}, {"maximum", 120}}},
				{"purpose", {{"type", "string"}, {"description", "Optional purpose ID from get_budget per_building."}}} }),
				json::array({ "x", "z", "percent" })), Permission::Control,
			Guarded([](const json& a) { return PreviewBuildingFunding(a.at("x").get<int32_t>(), a.at("z").get<int32_t>(), a.at("percent").get<double>(), a.contains("purpose") ? ParseId(a["purpose"]) : 0); },
				[](const json& a) { return SetBuildingFunding(a.at("x").get<int32_t>(), a.at("z").get<int32_t>(), a.at("percent").get<double>(), a.contains("purpose") ? ParseId(a["purpose"]) : 0); },
				[](const json& p) { return "Set the funding of " + p.value("building", std::string("the building")) + " to " + Number(p["new_percent"]) + "%?"; }) });

		tools.push_back({ "set_tax",
			"Sets a tax rate (0-20 %, 0.1 steps): type is one of r_low, r_medium, r_high, cs_low, cs_medium, cs_high, co_medium, co_high, i_agriculture, i_dirty, i_manufacturing, i_hightech, or residential, commercial, industrial, all. Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"type", {{"type", "string"}}}, {"rate", {{"type", "number"}, {"minimum", 0}, {"maximum", 20}}} }), json::array({ "type", "rate" })), Permission::Control,
			Guarded(PreviewTax, SetTax,
				[](const json& p) { return "Set " + std::to_string(p["changes"].size()) + " tax rate(s) to " + Number(p["rate"]) + "%?"; }) });

		tools.push_back({ "set_ordinance",
			"Turns an ordinance on or off: ordinance is its ID or part of its name (list_ordinances lists them). Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"ordinance", {{"type", "string"}}}, {"on", {{"type", "boolean"}}} }), json::array({ "ordinance", "on" })), Permission::Control,
			Guarded(PreviewOrdinance, SetOrdinance,
				[](const json& p) { return std::string(p.value("turn_on", false) ? "Enact " : "Repeal ") + p.value("name", std::string("the ordinance")) + "?"; }) });

		tools.push_back({ "save_city",
			"Saves the open city, as Ctrl+S does.",
			Schema(json::object()), Permission::Control, GameThread([](const json&) { return SaveCity(); }) });

		tools.push_back({ "place_zone",
			"Zones (or dezones, zone=none) a rectangle of cells, like the game's zone tools: zone is residential_low/medium/high, commercial_low/medium/high, agriculture, industrial_medium/high or none. Developed lots in the rectangle are refused unless allow_demolition=true, because changing their zone demolishes them; plopped buildings are never touched. At most 64 x 64 cells. Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"zone", {{"type", "string"}}}, {"x1", {{"type", "integer"}}}, {"z1", {{"type", "integer"}}}, {"x2", {{"type", "integer"}}}, {"z2", {{"type", "integer"}}},
				{"allow_demolition", {{"type", "boolean"}}} }), json::array({ "zone", "x1", "z1" })), Permission::Build,
			Guarded(PreviewZone, PlaceZone,
				[](const json& p)
				{
					std::string text = "Zone " + std::to_string(p.value("cells", 0)) + " cells as " + p.value("zone", std::string()) + "?";
					const size_t lots = p.value("lots_demolished", static_cast<size_t>(0));
					if (lots) text += " " + std::to_string(lots) + " developed lot(s) will be DEMOLISHED.";
					return text;
				}) });
	}
}
