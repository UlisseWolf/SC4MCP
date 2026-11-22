// SC4MCP - read-only tools: demand, advisor data, history, utilities, services, cells, maps, lots, ordinances,
// advisors.
//
// Simulators without an interface in gzcom-dll (plumbing, education, health) are read through the advisor Lua table
// `game`, which the game's advisor data pump fills every month; SC4SIM's graphs read the same fields.
#include "ToolsCommon.h"

#include "cIGZDate.h"
#include "cIGZPersistResourceManager.h"
#include "cIGZString.h"
#include "cIGZVariant.h"
#include "cISC4Advice.h"
#include "cISC4Advisor.h"
#include "cISC4AdvisorSystem.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4BuildingDevelopmentSimulator.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4City.h"
#include "cISC4CrimeSimulator.h"
#include "cISC4Demand.h"
#include "cISC4DemandSimulator.h"
#include "cISC4FireProtectionSimulator.h"
#include "cISC4HistoryWarehouse.h"
#include "cISC4LandValueSimulator.h"
#include "cISC4Lot.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISC4Ordinance.h"
#include "cISC4OrdinanceSimulator.h"
#include "cISC4PoliceSimulator.h"
#include "cISC4PollutionSimulator.h"
#include "cISC4PowerSimulator.h"
#include "cISC4SimGrid.h"
#include "cISC4Simulator.h"
#include "cISC4TrafficSimulator.h"
#include "cISC4ZoneManager.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
#include "cISCResExemplar.h"
#include "cGZPersistResourceKey.h"
#include "cRZAutoRefCount.h"
#include "GZServPtrs.h"
#include "SC4List.h"
#include "SC4Percentage.h"
#include "SC4Rect.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	// ------------------------------------------------------------------------------------------------- ids

	constexpr uint32_t kExemplarType = 0x6534284A;
	constexpr uint32_t kGraphGroup = 0xCA4AD545;
	constexpr uint32_t kTextGroup = 0x6A231EAA;
	constexpr uint32_t kPropExemplarName = 0x00000020;
	constexpr uint32_t kPropUserVisibleName = 0x8A416A99;
	constexpr uint32_t kPropGraphSources = 0x6A4AEE40;
	constexpr uint32_t kPropGraphLabels = 0x6A4AEEDC;

	constexpr uint32_t kPropStudents = 0x8922041B;
	constexpr uint32_t kPropPatients = 0x69220415;
	constexpr uint32_t kPropSchoolRadius = 0x691B42B3;
	constexpr uint32_t kPropHospitalRadius = 0x091B42A2;
	constexpr uint32_t kOccupantGroupPower = 0x1400;

	struct DemandType { uint32_t id; const char* label; };
	constexpr DemandType kDemands[] = {
		{0x1010, "R$"}, {0x1020, "R$$"}, {0x1030, "R$$$"},
		{0x3110, "Cs$"}, {0x3120, "Cs$$"}, {0x3130, "Cs$$$"}, {0x3320, "Co$$"}, {0x3330, "Co$$$"},
		{0x4100, "I-R"}, {0x4200, "I-D"}, {0x4300, "I-M"}, {0x4400, "I-HT"} };
	constexpr uint32_t kTotalsIndex = 0x20000;

	// ------------------------------------------------------------------------------------------------- helpers

	std::vector<uint32_t> Uint32Values(const cISCPropertyHolder* holder, uint32_t id)
	{
		std::vector<uint32_t> out;
		const cISCProperty* property = holder ? holder->GetProperty(id) : nullptr;
		const cIGZVariant* value = property ? property->GetPropertyValue() : nullptr;
		if (!value) return out;
		const uint32_t count = value->GetCount();
		const uint16_t type = value->GetType();
		if (type == cIGZVariant::Uint32Array || type == cIGZVariant::Sint32Array)
		{
			const uint32_t* data = value->RefUint32();
			if (data) out.assign(data, data + count);
		}
		else
		{
			uint32_t single = 0;
			if (value->GetValUint32(single)) out.push_back(single);
		}
		return out;
	}

	std::string StringValue(const cISCPropertyHolder* holder, uint32_t id)
	{
		cRZBaseString text;
		if (holder && holder->GetProperty(id, text)) return text.ToChar();
		return {};
	}

	uint32_t Uint32Value(const cISCPropertyHolder* holder, uint32_t id, bool& found)
	{
		uint32_t value = 0;
		found = holder && holder->GetProperty(id, value);
		return value;
	}

	double Number(const GameTable& table, const char* name)
	{
		const auto found = table.numbers.find(name);
		return found == table.numbers.end() ? std::numeric_limits<double>::quiet_NaN() : found->second;
	}

	void PutNumber(json& out, const char* key, double value)
	{
		if (std::isnan(value)) return;
		if (value == std::floor(value) && std::fabs(value) < 9.0e15) out[key] = static_cast<int64_t>(value);
		else out[key] = value;
	}

	int DaysInMonth(int month, int year)
	{
		static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
		return days[(month - 1) % 12];
	}

	// ------------------------------------------------------------------------------------------------- demand

	json GetDemand()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4DemandSimulator* simulator = city->GetDemandSimulator();
		if (!simulator) return Error("Demand simulator unavailable.");
		json types = json::object();
		for (const auto& type : kDemands)
		{
			const cISC4Demand* demand = simulator->GetDemand(type.id, kTotalsIndex);
			if (!demand) demand = simulator->GetDemand(type.id, 0);
			if (!demand) continue;
			json d{ {"id", Hex(type.id)}, {"demand", demand->QueryDemandValue()}, {"supply", demand->QuerySupplyValue()},
				{"active_demand", demand->QueryActiveDemandValue()}, {"active_demand_min", demand->QueryActiveDemandMin()},
				{"active_demand_max", demand->QueryActiveDemandMax()}, {"new_demand", demand->QueryNewDemand()},
				{"new_supply", demand->QueryNewSupply()}, {"economy_modifier", demand->GetEconomyModifier()},
				{"tax_modifier", demand->GetTaxModifier()} };
			// The demand cap percentage is only maintained for agriculture (I-R): the other types are capped through
			// the caps of the advisor data below, so it is reported for I-R alone.
			if (type.id == 0x4100)
			{
				if (const SC4Percentage* cap = demand->GetDemandCap()) d["cap_percent"] = cap->percentage * 100.0f;
			}
			types[type.label] = d;
		}
		json out{ {"types", types}, {"neutral_tax_rate", simulator->GetNeutralTaxRate()} };
		GameTable table;
		if (ReadGameTable(city, table)) out["advisor_data"] = GameFields(table, { "cap", "demand" });
		out["note"] = "demand and supply are the city totals (index 0x20000). Caps come from the advisor data fields whose name contains 'cap'.";
		return out;
	}

	// ------------------------------------------------------------------------------------------------- game data

	json GetGameData(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		GameTable table;
		if (!ReadGameTable(city, table)) return Error("The advisor data table is not available yet (open the city and let one month pass).");
		std::vector<std::string> words;
		if (arguments.contains("contains"))
		{
			if (arguments["contains"].is_string()) words.push_back(Lower(arguments["contains"].get<std::string>()));
			else if (arguments["contains"].is_array())
			{
				for (const json& word : arguments["contains"]) if (word.is_string()) words.push_back(Lower(word.get<std::string>()));
			}
		}
		json fields = GameFields(table, words);
		const size_t limit = static_cast<size_t>(std::clamp(arguments.value("limit", 500), 1, 5000));
		json trimmed = json::object();
		size_t count = 0;
		for (auto it = fields.begin(); it != fields.end() && count < limit; ++it, ++count) trimmed[it.key()] = it.value();
		return json{ {"total_fields", table.numbers.size() + table.strings.size()}, {"matching", fields.size()}, {"returned", trimmed.size()}, {"fields", trimmed} };
	}

	// ------------------------------------------------------------------------------------------------- graphs

	struct GraphInfo
	{
		uint32_t instance = 0;
		std::string name;
		std::string title;
		std::vector<uint32_t> sources;
		std::vector<std::string> labels;
	};

	bool ReadGraph(cIGZPersistResourceManager* resources, uint32_t instance, GraphInfo& out)
	{
		cRZAutoRefCount<cISCResExemplar> exemplar;
		const cGZPersistResourceKey key(kExemplarType, kGraphGroup, instance);
		if (!resources->TestForKey(key)) return false;
		if (!resources->GetResource(key, GZIID_cISCResExemplar, exemplar.AsPPVoid(), 0, nullptr) || !exemplar) return false;
		const cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
		out.instance = instance;
		out.name = StringValue(holder, kPropExemplarName);
		const std::vector<uint32_t> title = Uint32Values(holder, kPropUserVisibleName);
		if (title.size() >= 3) out.title = LocalizedText(title[1], title[2]);
		out.sources = Uint32Values(holder, kPropGraphSources);
		for (uint32_t label : Uint32Values(holder, kPropGraphLabels)) out.labels.push_back(LocalizedText(kTextGroup, label));
		return !out.sources.empty();
	}

	json ListGraphs()
	{
		cIGZPersistResourceManagerPtr resources;
		if (!resources) return Error("Resource manager unavailable.");
		json list = json::array();
		for (uint32_t instance = 1; instance <= 0xFF; ++instance)
		{
			GraphInfo graph;
			if (!ReadGraph(resources, instance, graph)) continue;
			json series = json::array();
			for (size_t i = 0; i < graph.sources.size(); ++i)
			{
				series.push_back(json{ {"history_type", Hex(graph.sources[i])}, {"label", i < graph.labels.size() ? graph.labels[i] : std::string()} });
			}
			list.push_back(json{ {"graph", Hex(instance)}, {"name", graph.name}, {"title", graph.title}, {"series", series} });
		}
		return json{ {"graphs", list} };
	}

	json GetHistory(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4HistoryWarehouse* history = city->GetHistoryWarehouse();
		cISC4Simulator* simulator = city->GetSimulator();
		if (!history || !simulator || !simulator->GetSimDate()) return Error("History warehouse unavailable.");

		std::vector<std::pair<uint32_t, std::string>> series;
		json graphInfo = nullptr;
		if (arguments.contains("graph"))
		{
			cIGZPersistResourceManagerPtr resources;
			GraphInfo graph;
			if (!resources || !ReadGraph(resources, ParseId(arguments["graph"]), graph)) return Error("No graph exemplar with that instance (list_graphs lists them).");
			for (size_t i = 0; i < graph.sources.size(); ++i) series.push_back({ graph.sources[i], i < graph.labels.size() ? graph.labels[i] : std::string() });
			graphInfo = json{ {"graph", Hex(graph.instance)}, {"name", graph.name}, {"title", graph.title} };
		}
		if (arguments.contains("history_types") && arguments["history_types"].is_array())
		{
			for (const json& id : arguments["history_types"]) series.push_back({ ParseId(id), std::string() });
		}
		if (series.empty()) return Error("Give graph (a graph instance from list_graphs) or history_types (a list of history type IDs).");
		if (series.size() > 32) series.resize(32);

		const int months = std::clamp(arguments.value("months", 12), 1, 600);
		uint32_t year = 0, month = 0, day = 0, dayOfYear = 0, weekDay = 0;
		simulator->GetSimDate(&year, &month, &day, &dayOfYear, &weekDay);

		cIGZDate* start = nullptr;
		cIGZDate* end = nullptr;
		if (!simulator->GetSimDate()->Clone(&start) || !simulator->GetSimDate()->Clone(&end) || !start || !end)
		{
			if (start) start->Release();
			if (end) end->Release();
			return Error("Cannot create dates.");
		}

		json dates = json::array();
		std::vector<json> values(series.size(), json::array());
		int y = static_cast<int>(year), m = static_cast<int>(month);
		for (int i = 0; i < months - 1; ++i) { if (--m < 1) { m = 12; --y; } }
		for (int i = 0; i < months; ++i)
		{
			start->Set(static_cast<uint32_t>(m), 1u, static_cast<uint32_t>(y));
			end->Set(static_cast<uint32_t>(m), static_cast<uint32_t>(DaysInMonth(m, y)), static_cast<uint32_t>(y));
			char label[16];
			std::snprintf(label, sizeof(label), "%04d-%02d", y, m);
			dates.push_back(label);
			for (size_t s = 0; s < series.size(); ++s)
			{
				float minimum = 0, maximum = 0, average = 0;
				if (history->GetMinMaxAverage(series[s].first, *start, *end, minimum, maximum, average) && std::isfinite(average)) values[s].push_back(average);
				else values[s].push_back(nullptr);
			}
			if (++m > 12) { m = 1; ++y; }
		}
		start->Release();
		end->Release();

		json out{ {"months", dates} };
		if (!graphInfo.is_null()) out["graph"] = graphInfo;
		json list = json::array();
		for (size_t s = 0; s < series.size(); ++s)
		{
			list.push_back(json{ {"history_type", Hex(series[s].first)}, {"label", series[s].second}, {"values", values[s]} });
		}
		out["series"] = list;
		out["note"] = "Monthly averages of the city history (null = no data for that month). Lines recorded by a DLL start the month after it was installed.";
		return out;
	}

	// ------------------------------------------------------------------------------------------------- utilities

	json GetUtilities(bool perPlant)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		json out = json::object();

		if (cISC4PowerSimulator* power = city->GetPowerSimulator())
		{
			json p{ {"monthly_production", power->GetMonthlyProduction()}, {"monthly_consumption", power->GetMonthlyConsumption()},
				{"monthly_demand", power->GetMonthlyDemand()}, {"monthly_requirement", power->GetMonthlyRequirement()},
				{"monthly_import", power->GetMonthlyImport()}, {"monthly_export", power->GetMonthlyExport()},
				{"powered_consumers", power->GetPoweredConsumerCount()}, {"unpowered_consumers", power->GetUnpoweredConsumerCount()},
				{"plants", power->GetPowerPlantCount()}, {"overworked_plants", power->GetOverworkedPowerPlantCount()},
				{"aged_plants", power->GetAgedPowerPlantCount()}, {"distressed_plants", power->GetDistressedPowerPlantCount()},
				{"power_poles", power->GetPowerPoleCount()} };
			const int32_t production = power->GetMonthlyProduction();
			if (production > 0) p["usage_percent"] = 100.0 * power->GetMonthlyConsumption() / production;
			if (perPlant)
			{
				json plants = json::array();
				for (cISC4Occupant* occupant : AllBuildings(city))
				{
					if (!occupant->IsOccupantGroup(kOccupantGroupPower)) continue;
					uint32_t produced = 0, used = 0, maximum = 0;
					power->GetOccupantPowerProduced(occupant, produced, used);
					power->GetOccupantMaxPowerProduced(occupant, maximum);
					if (maximum == 0 && produced == 0) continue; // power poles, substations
					json plant{ {"name", BuildingName(occupant)}, {"exemplar", Hex(BuildingType(occupant))}, {"cells", BuildingCells(occupant)},
						{"produced", produced}, {"used", used}, {"max_output", maximum},
						{"efficiency_percent", power->GetCurrentPlantEfficiencyPercent(occupant)},
						{"condition_percent", power->GetCurrentPlantConditionPercent(occupant)} };
					if (maximum > 0) plant["load_percent"] = 100.0 * used / maximum;
					int64_t cost = 0;
					if (power->GetMonthlyPowerGenerationCost(occupant, cost)) plant["monthly_generation_cost"] = cost;
					plants.push_back(plant);
				}
				p["plant_list"] = plants;
			}
			out["power"] = p;
		}

		if (cISC4PollutionSimulator* pollution = city->GetPollutionSimulator())
		{
			out["garbage"] = json{ {"produced_last_month", pollution->GetGarbageProducedLastMonth()},
				{"landfilled_last_month", pollution->GetGarbageLandfilledLastMonth()},
				{"recycled_last_month", pollution->GetGarbageRecycledLastMonth()},
				{"incinerated_last_month", pollution->GetGarbageIncineratedLastMonth()},
				{"converted_to_energy_last_month", pollution->GetGarbageConvertedToEnergyLastMonth()},
				{"imported_last_month", pollution->GetGarbageImportedLastMonth()},
				{"exported_last_month", pollution->GetGarbageExportedLastMonth()},
				{"uncollected_total", pollution->GetTotalGarbageUncollected()}, {"uncollected", pollution->IsGarbageUncollected()},
				{"capacity", pollution->GetGarbageCapacity()}, {"landfill_tile_capacity", pollution->GetLandfillTileCapacity()},
				{"monthly_incinerator_capacity", pollution->GetMonthlyIncineratorCapacity()},
				{"monthly_waste_to_energy_capacity", pollution->GetMonthlyWasteToEnergyCapacity()},
				{"incinerators", pollution->GetIncineratorCount()}, {"waste_to_energy_plants", pollution->GetWasteToEnergyBuildingCount()},
				{"recycling_centers", pollution->GetRecyclingCenterCount()} };
			out["water_treatment_plants"] = pollution->GetWaterTreatmentPlantCount();
		}

		// The plumbing simulator has no interface in gzcom-dll: water comes from the advisor data.
		GameTable table;
		if (ReadGameTable(city, table))
		{
			out["water_advisor_data"] = GameFields(table, { "water", "pipe", "plumb", "sewage", "pump" });
			out["power_advisor_data"] = GameFields(table, { "power", "electric" });
		}
		return out;
	}

	// ------------------------------------------------------------------------------------------------- services

	json GetServices(bool perBuilding)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		json out = json::object();

		GameTable table;
		const bool haveTable = ReadGameTable(city, table);
		if (haveTable)
		{
			json education = json::object();
			PutNumber(education, "coverage_percent", Number(table, "g_education_coverage_p"));
			PutNumber(education, "average_eq", Number(table, "ga_education"));
			PutNumber(education, "school_grade_percent", Number(table, "ga_school_grade"));
			PutNumber(education, "library_grade_percent", Number(table, "ga_library_grade"));
			PutNumber(education, "museum_grade_percent", Number(table, "ga_museum_grade"));
			PutNumber(education, "best_neighbourhood_eq", Number(table, "l_tract_eq_h"));
			PutNumber(education, "worst_neighbourhood_eq", Number(table, "l_tract_eq_l"));
			education["advisor_data"] = GameFields(table, { "school", "educ", "student", "librar", "museum", "college", "universit" });
			out["education"] = education;

			json health = json::object();
			PutNumber(health, "coverage_percent", Number(table, "g_health_coverage_p"));
			PutNumber(health, "average_health", Number(table, "ga_health"));
			PutNumber(health, "health_grade_percent", Number(table, "ga_health_grade"));
			PutNumber(health, "treated_patients", Number(table, "g_num_treated_patients"));
			PutNumber(health, "capacity", Number(table, "g_health_capacity"));
			PutNumber(health, "hospitals", Number(table, "g_num_hospitals"));
			PutNumber(health, "clinics", Number(table, "g_num_clinics"));
			PutNumber(health, "life_expectancy", Number(table, "ga_life_exp"));
			PutNumber(health, "best_neighbourhood_life_expectancy", Number(table, "l_tract_life_exp_h"));
			PutNumber(health, "worst_neighbourhood_life_expectancy", Number(table, "l_tract_life_exp_l"));
			const double treated = Number(table, "g_num_treated_patients"), capacity = Number(table, "g_health_capacity");
			if (!std::isnan(treated) && capacity > 0) health["occupancy_percent"] = 100.0 * treated / capacity;
			health["advisor_data"] = GameFields(table, { "health", "hospital", "clinic", "patient", "life_exp" });
			out["health"] = health;
		}
		else
		{
			out["note"] = "The advisor data table is not available yet: education and health are missing (let one month pass).";
		}

		if (cISC4PoliceSimulator* police = city->GetPoliceSimulator())
		{
			out["police"] = json{ {"stations", police->GetPoliceStationCount()}, {"coverage_percent", police->GetPercentCoverage()},
				{"average_coverage", police->GetAverageCoverage()}, {"criminals", police->GetCriminalCount()},
				{"arrests", police->GetArrestCount()}, {"dispatches", police->GetTotalDispatchCount()}, {"on_strike", police->IsOnStrike()} };
			out["jails"] = json{ {"jails", police->GetJailCount()}, {"capacity", police->GetJailCapacity()},
				{"inmates", police->GetJailPopulation()}, {"jailed_percent", police->GetJailedPercentage()},
				{"monthly_upkeep", police->GetJailCurrentUpkeepCost()} };
		}
		if (cISC4CrimeSimulator* crime = city->GetCrimeSimulator())
		{
			out["crime"] = json{ {"average", crime->GetAverageCrime()}, {"monthly_crimes", crime->GetMonthlyCrimeCount()} };
		}
		if (cISC4FireProtectionSimulator* fire = city->GetFireProtectionSimulator())
		{
			out["fire"] = json{ {"stations", fire->GetFireStationCount()}, {"coverage_percent", fire->GetPercentCoverage()},
				{"average_coverage", fire->GetAverageCoverage()}, {"dispatches", fire->GetTotalDispatchCount()}, {"on_strike", fire->IsOnStrike()} };
		}

		if (perBuilding)
		{
			cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
			cISC4PoliceSimulator* police = city->GetPoliceSimulator();
			cISC4FireProtectionSimulator* fire = city->GetFireProtectionSimulator();
			json schools = json::array(), hospitals = json::array(), stations = json::array(), jails = json::array(), fireStations = json::array();
			for (cISC4Occupant* occupant : AllBuildings(city))
			{
				const cISCPropertyHolder* holder = occupant->AsPropertyHolder();
				bool hasStudents = false, hasPatients = false, found = false;
				const uint32_t students = Uint32Value(holder, kPropStudents, hasStudents);
				const uint32_t patients = Uint32Value(holder, kPropPatients, hasPatients);
				const bool isPolice = police && police->IsPoliceStation(occupant);
				const bool isJail = police && police->IsJail(occupant);
				const bool isFire = fire && fire->IsFireStation(occupant);
				if (!hasStudents && !hasPatients && !isPolice && !isJail && !isFire) continue;
				json b{ {"name", BuildingName(occupant)}, {"exemplar", Hex(BuildingType(occupant))}, {"cells", BuildingCells(occupant)} };
				const BuildingCost cost = CostOf(budget, occupant);
				if (!cost.items.empty()) b["monthly_cost"] = cost.monthly;
				if (hasStudents)
				{
					b["student_capacity"] = students;
					const uint32_t radius = Uint32Value(holder, kPropSchoolRadius, found);
					if (found) b["radius"] = radius;
					schools.push_back(b);
				}
				else if (hasPatients)
				{
					b["patient_capacity"] = patients;
					const uint32_t radius = Uint32Value(holder, kPropHospitalRadius, found);
					if (found) b["radius"] = radius;
					hospitals.push_back(b);
				}
				else if (isJail)
				{
					b["capacity"] = police->GetJailCapacity(occupant);
					jails.push_back(b);
				}
				else if (isPolice)
				{
					b["efficiency"] = police->GetStationEfficiency(occupant);
					b["dispatch_radius"] = police->GetStationDispatchRadius(occupant);
					b["arrests"] = police->GetPoliceStationArrestCount(occupant);
					b["crimes"] = police->GetPoliceStationCrimeCount(occupant);
					stations.push_back(b);
				}
				else if (isFire)
				{
					b["dispatch_radius"] = fire->GetStationDispatchRadius(occupant);
					fireStations.push_back(b);
				}
			}
			out["buildings"] = json{ {"schools", schools}, {"hospitals", hospitals}, {"police_stations", stations}, {"jails", jails}, {"fire_stations", fireStations} };
			out["buildings_note"] = "Capacities and radii are the building properties (Students, Patients, coverage radius); current enrolment per building is not exposed by the game.";
		}
		return out;
	}

	// ------------------------------------------------------------------------------------------------- cells and maps

	const char* StateName(uint32_t state)
	{
		static const char* const names[] = { "created", "ready", "constructing", "occupied", "vacant", "destructing", "destroyed" };
		return state < 7 ? names[state] : "unknown";
	}

	json LotInfo(cISC4City* city, cISC4Lot* lot, cISC4Occupant* building)
	{
		json l = json::object();
		SC4Rect<int32_t> rect{};
		if (lot->GetBoundingRect(rect)) l["cells"] = json{ {"x1", rect.topLeftX}, {"z1", rect.topLeftY}, {"x2", rect.bottomRightX}, {"z2", rect.bottomRightY} };
		l["zone"] = ZoneName(static_cast<int32_t>(lot->GetZoneType()));
		l["state"] = StateName(static_cast<uint32_t>(lot->GetState()));
		l["occupancy_percent"] = lot->GetOccupancy() * 100.0f;
		l["wealth"] = lot->GetOccupantWealth();
		l["land_value"] = lot->GetLandValue();
		l["condition"] = lot->GetCondition();
		l["needs_power"] = lot->GetNeedsPower();
		l["has_power"] = lot->GetHasPower();
		l["road_access"] = lot->HasRoadAccess();
		l["job_access"] = lot->HasJobAccess();
		l["historical"] = lot->IsHistorical();
		json population = json::object();
		static const std::pair<uint32_t, const char*> kTypes[] = {
			{0x1010, "R$"}, {0x1020, "R$$"}, {0x1030, "R$$$"}, {0x3110, "Cs$"}, {0x3120, "Cs$$"}, {0x3130, "Cs$$$"},
			{0x3320, "Co$$"}, {0x3330, "Co$$$"}, {0x4100, "I-R"}, {0x4200, "I-D"}, {0x4300, "I-M"}, {0x4400, "I-HT"} };
		for (const auto& [id, label] : kTypes)
		{
			const auto type = static_cast<cISC4BuildingDevelopmentSimulator::DeveloperType>(id);
			const uint16_t capacity = lot->GetCapacity(type, false);
			const uint16_t present = lot->GetPopulation(type);
			if (capacity || present) population[label] = json{ {"population", present}, {"capacity", capacity} };
		}
		if (!population.empty()) l["population"] = population;
		if (building)
		{
			l["building"] = BuildingName(building);
			l["exemplar"] = Hex(BuildingType(building));
			const BuildingCost cost = CostOf(city->GetBudgetSimulator(), building);
			if (!cost.items.empty()) l["monthly_cost"] = cost.monthly;
		}
		return l;
	}

	template<typename T>
	json GridValue(cISC4SimGrid<T>* grid, int32_t x, int32_t z)
	{
		if (!grid) return nullptr;
		return static_cast<int64_t>(grid->GetCellValue(x, z));
	}

	json QueryCell(int32_t x, int32_t z)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		if (!InCity(city, x, z)) return Error("Cell outside the city (size " + std::to_string(city->CellCountX()) + " x " + std::to_string(city->CellCountZ()) + ").");
		json out{ {"x", x}, {"z", z} };
		if (cISC4ZoneManager* zones = city->GetZoneManager())
		{
			out["zone"] = ZoneName(static_cast<int32_t>(zones->GetZoneType(x, z)));
			out["development_failures"] = zones->GetDevelopmentFailureCount(x, z);
		}
		cISC4Lot* lot = nullptr;
		cISC4Occupant* building = BuildingAt(city, x, z, &lot);
		out["lot"] = lot ? LotInfo(city, lot, building) : json(nullptr);
		if (cISC4LandValueSimulator* landValue = city->GetLandValueSimulator())
		{
			out["land_value"] = landValue->GetLandValue(x, z);
			static const char* const types[] = { "", "low", "medium", "high" };
			const uint32_t type = static_cast<uint32_t>(landValue->GetLandValueType(x, z));
			out["land_value_type"] = type <= 3 ? types[type] : "unknown";
		}
		if (cISC4PollutionSimulator* pollution = city->GetPollutionSimulator())
		{
			int32_t air = 0, water = 0, garbage = 0;
			json p = json::object();
			if (pollution->GetAirValue(x, z, air)) p["air"] = air;
			if (pollution->GetWaterValue(x, z, water)) p["water"] = water;
			if (pollution->GetGarbageValue(x, z, garbage)) p["garbage"] = garbage;
			p["radioactive"] = pollution->IsRadioactive(x, z);
			p["active_landfill"] = pollution->IsActiveLandfill(x, z);
			out["pollution"] = p;
		}
		if (cISC4CrimeSimulator* crime = city->GetCrimeSimulator()) out["crime"] = GridValue(crime->GetCrimeGrid(), x, z);
		if (cISC4PoliceSimulator* police = city->GetPoliceSimulator()) out["police_coverage"] = GridValue(police->GetPolicePowerGrid(), x, z);
		if (cISC4FireProtectionSimulator* fire = city->GetFireProtectionSimulator()) out["fire_protection"] = GridValue(fire->GetFireProtectionGrid(), x, z);
		if (cISC4PowerSimulator* power = city->GetPowerSimulator())
		{
			out["powered"] = power->IsPowered(x, z);
			out["power_consumption"] = power->GetMonthlyConsumption(x, z);
		}
		if (cISC4TrafficSimulator* traffic = city->GetTrafficSimulator()) out["congestion"] = GridValue(traffic->GetCongestionMap(), x, z);
		return out;
	}

	struct MapDef
	{
		const char* name;
		bool higherIsWorse;
		std::function<bool(cISC4City*, int32_t, int32_t, double&)> read;
	};

	const std::vector<MapDef>& Maps()
	{
		static const std::vector<MapDef> maps = {
			{ "land_value", false, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetLandValueSimulator(); if (!s) return false; v = s->GetLandValue(x, z); return true; } },
			{ "crime", true, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetCrimeSimulator(); auto* g = s ? s->GetCrimeGrid() : nullptr; if (!g) return false; v = g->GetCellValue(x, z); return true; } },
			{ "air_pollution", true, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetPollutionSimulator(); int32_t n = 0; if (!s || !s->GetAirValue(x, z, n)) return false; v = n; return true; } },
			{ "water_pollution", true, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetPollutionSimulator(); int32_t n = 0; if (!s || !s->GetWaterValue(x, z, n)) return false; v = n; return true; } },
			{ "garbage", true, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetPollutionSimulator(); int32_t n = 0; if (!s || !s->GetGarbageValue(x, z, n)) return false; v = n; return true; } },
			{ "fire_protection", false, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetFireProtectionSimulator(); auto* g = s ? s->GetFireProtectionGrid() : nullptr; if (!g) return false; v = g->GetCellValue(x, z); return true; } },
			{ "police_coverage", false, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetPoliceSimulator(); auto* g = s ? s->GetPolicePowerGrid() : nullptr; if (!g) return false; v = g->GetCellValue(x, z); return true; } },
			{ "congestion", true, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetTrafficSimulator(); auto* g = s ? s->GetCongestionMap() : nullptr; if (!g) return false; v = g->GetCellValue(x, z); return true; } },
			{ "powered", false, [](cISC4City* c, int32_t x, int32_t z, double& v) { auto* s = c->GetPowerSimulator(); if (!s) return false; v = s->IsPowered(x, z) ? 1 : 0; return true; } },
		};
		return maps;
	}

	json GetMapStats(const std::string& name, int top, bool developedOnly)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		const int32_t sizeX = static_cast<int32_t>(city->CellCountX()), sizeZ = static_cast<int32_t>(city->CellCountZ());

		if (name == "zones")
		{
			cISC4ZoneManager* zones = city->GetZoneManager();
			if (!zones) return Error("Zone manager unavailable.");
			json counts = json::object();
			for (int32_t type = 1; type <= 15; ++type)
			{
				const int32_t count = zones->GetZoneCount(static_cast<cISC4ZoneManager::ZoneType>(type));
				if (count) counts[ZoneName(type)] = json{ {"cells", count}, {"undeveloped", zones->GetUndevelopedTileCount(static_cast<cISC4ZoneManager::ZoneType>(type))} };
			}
			return json{ {"map", "zones"}, {"zones", counts}, {"undeveloped_cells", zones->GetUndevelopedTileCount()}, {"abandoned_cells", zones->GetAbandonedTileCount()} };
		}

		const MapDef* map = nullptr;
		for (const MapDef& candidate : Maps()) if (name == candidate.name) map = &candidate;
		if (!map)
		{
			json names = json::array({ "zones" });
			for (const MapDef& candidate : Maps()) names.push_back(candidate.name);
			return json{ {"error", "Unknown map."}, {"maps", names} };
		}

		cISC4LotManager* lots = city->GetLotManager();
		struct Cell { double value; int32_t x; int32_t z; };
		std::vector<Cell> cells;
		cells.reserve(static_cast<size_t>(sizeX) * sizeZ);
		for (int32_t x = 0; x < sizeX; ++x)
		{
			for (int32_t z = 0; z < sizeZ; ++z)
			{
				if (developedOnly && (!lots || !lots->GetLot(x, z, false))) continue;
				double value = 0;
				if (map->read(city, x, z, value) && std::isfinite(value)) cells.push_back({ value, x, z });
			}
		}
		if (cells.empty()) return json{ {"map", name}, {"cells", 0} };

		double sum = 0, minimum = cells.front().value, maximum = cells.front().value;
		for (const Cell& cell : cells) { sum += cell.value; minimum = std::min(minimum, cell.value); maximum = std::max(maximum, cell.value); }
		const double average = sum / cells.size();
		json histogram = json::array();
		constexpr int kBuckets = 10;
		std::vector<size_t> buckets(kBuckets, 0);
		const double width = (maximum - minimum) / kBuckets;
		for (const Cell& cell : cells)
		{
			int bucket = width > 0 ? static_cast<int>((cell.value - minimum) / width) : 0;
			buckets[std::clamp(bucket, 0, kBuckets - 1)]++;
		}
		for (int i = 0; i < kBuckets; ++i)
		{
			histogram.push_back(json{ {"from", minimum + width * i}, {"to", minimum + width * (i + 1)}, {"cells", buckets[i]} });
		}
		std::sort(cells.begin(), cells.end(), [&](const Cell& a, const Cell& b) { return map->higherIsWorse ? a.value > b.value : a.value < b.value; });
		json worst = json::array();
		for (int i = 0; i < top && i < static_cast<int>(cells.size()); ++i) worst.push_back(json{ {"x", cells[i].x}, {"z", cells[i].z}, {"value", cells[i].value} });
		return json{ {"map", name}, {"cells", cells.size()}, {"developed_only", developedOnly}, {"min", minimum}, {"max", maximum}, {"average", average},
			{"histogram", histogram}, {"worst_cells", worst}, {"worst_means", map->higherIsWorse ? "highest" : "lowest"} };
	}

	// ------------------------------------------------------------------------------------------------- lots

	json ListLots(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4LotManager* lots = city->GetLotManager();
		if (!lots) return Error("Lot manager unavailable.");
		const int32_t zone = arguments.contains("zone") ? ParseZone(arguments["zone"]) : -1;
		if (arguments.contains("zone") && zone < 0) return Error("Unknown zone.");
		const std::string state = Lower(arguments.value("state", std::string()));
		const bool filterPower = arguments.contains("powered");
		const bool powered = arguments.value("powered", true);
		const size_t limit = static_cast<size_t>(std::clamp(arguments.value("limit", 100), 1, 2000));

		json list = json::array();
		size_t total = 0;
		std::set<cISC4Lot*> seen;
		for (cISC4Occupant* occupant : AllBuildings(city))
		{
			cISC4Lot* lot = lots->GetOccupantLot(occupant);
			if (!lot || !seen.insert(lot).second) continue;
			if (zone >= 0 && static_cast<int32_t>(lot->GetZoneType()) != zone) continue;
			const char* lotState = StateName(static_cast<uint32_t>(lot->GetState()));
			if (!state.empty())
			{
				const bool abandoned = lot->GetState() == cISC4Lot::HabitationState::Vacant;
				if (state == "abandoned" ? !abandoned : state != lotState) continue;
			}
			if (filterPower && (!lot->GetNeedsPower() || lot->GetHasPower() != powered)) continue;
			++total;
			if (list.size() < limit) list.push_back(LotInfo(city, lot, occupant));
		}
		return json{ {"total", total}, {"returned", list.size()}, {"lots", list},
			{"note", "Lots with a building. state=abandoned lists vacant lots (the game's abandoned buildings)."} };
	}

	// ------------------------------------------------------------------------------------------------- ordinances

	json ListOrdinances()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4OrdinanceSimulator* ordinances = city->GetOrdinanceSimulator();
		if (!ordinances) return Error("Ordinance simulator unavailable.");
		uint32_t ids[512]{};
		uint32_t count = 512;
		ordinances->GetOrdinanceIDArray(ids, count);
		count = std::min<uint32_t>(count, 512);
		json list = json::array();
		for (uint32_t i = 0; i < count; ++i)
		{
			cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(ids[i]);
			if (!ordinance) continue;
			cIGZString* name = ordinance->GetName();
			cIGZString* description = ordinance->GetDescription();
			list.push_back(json{ {"id", Hex(ids[i])}, {"name", name ? name->ToChar() : ""},
				{"description", description ? description->ToChar() : ""},
				{"available", ordinance->IsAvailable()}, {"on", ordinance->IsOn()}, {"enabled", ordinance->IsEnabled()},
				{"income_ordinance", ordinance->IsIncomeOrdinance()}, {"current_monthly_income", ordinance->GetCurrentMonthlyIncome()},
				{"monthly_adjusted_income", ordinance->GetMonthlyAdjustedIncome()}, {"enactment_income", ordinance->GetEnactmentIncome()},
				{"year_first_available", ordinance->GetYearFirstAvailable()}, {"advisor", ordinance->GetAdvisorID()} });
		}
		return json{ {"ordinances", list}, {"monthly_expense", ordinances->GetOrdinanceMonthlyExpense()},
			{"monthly_income", ordinances->GetOrdinanceMonthlyIncome()} };
	}

	// ------------------------------------------------------------------------------------------------- advisors

	json AdviceInfo(cISC4Advice* advice)
	{
		const char* title = advice->GetTextTitle();
		const char* text = advice->GetTextMessage();
		return json{ {"guid", Hex(advice->GetGUID())}, {"priority", advice->GetPriority()}, {"mood", advice->GetMood()},
			{"title", title ? title : ""}, {"message", text ? text : ""} };
	}

	json GetAdvisors(int maxMessages)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4AdvisorSystem* system = city->GetAdvisorSystem();
		if (!system) return Error("Advisor system unavailable.");

		// Advisor IDs: the ones the ordinances name, plus the small IDs the game uses.
		std::set<int32_t> ids;
		for (int32_t id = 0; id < 64; ++id) ids.insert(id);
		if (cISC4OrdinanceSimulator* ordinances = city->GetOrdinanceSimulator())
		{
			uint32_t list[512]{};
			uint32_t count = 512;
			ordinances->GetOrdinanceIDArray(list, count);
			for (uint32_t i = 0; i < std::min<uint32_t>(count, 512); ++i)
			{
				if (cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(list[i])) ids.insert(static_cast<int32_t>(ordinance->GetAdvisorID()));
			}
		}

		json advisors = json::array();
		for (int32_t id : ids)
		{
			cISC4Advisor* advisor = system->GetAdvisor(id);
			if (!advisor) continue;
			json a{ {"id", Hex(static_cast<uint32_t>(id))} };
			if (cIGZString* name = advisor->GetName()) a["name"] = name->ToChar();
			if (cIGZString* person = advisor->GetPersonalName()) a["personal_name"] = person->ToChar();
			json messages = json::array();
			int n = 0;
			for (const auto& entry : advisor->GetCurrentAdvices())
			{
				if (n++ >= maxMessages) break;
				if (cISC4Advice* advice = entry.second) messages.push_back(AdviceInfo(advice));
			}
			a["messages"] = messages;
			advisors.push_back(a);
		}
		json out{ {"advisors", advisors}, {"popup_priority_threshold", system->GetAdvicePopupPriorityThreshold()} };
		if (cISC4Advice* context = system->GetContextAdvice()) out["current_popup"] = AdviceInfo(context);
		return out;
	}
}

namespace ToolsCommon
{
	void AddReadTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "get_demand",
			"RCI demand of every type (city totals): demand, supply, active demand and its range, new demand and supply, economy and tax modifiers; the neutral tax rate; and the cap and demand fields of the advisor data.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetDemand(); }) });

		tools.push_back({ "get_game_data",
			"Raw advisor data of the city (the Lua table `game` the advisors and UI read: coverage, grades, capacities, tax rates, counts...). Filter with contains (a word or a list of words).",
			Schema({ {"contains", {{"description", "Word or list of words the field name must contain, e.g. \"tax\" or [\"water\",\"pump\"]."}}},
				{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}} }), Permission::Read,
			GameThread([](const json& arguments) { return GetGameData(arguments); }) });

		tools.push_back({ "list_graphs",
			"Graphs of the Graphs window (Maxis, SC4SIM and other plugins): instance, title and the history type and label of every line. Use them with get_history.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return ListGraphs(); }) });

		tools.push_back({ "get_history",
			"Monthly history of the city, as plotted by the Graphs window: give graph (an instance from list_graphs) or history_types (a list of IDs), and months (default 12, up to 600).",
			Schema({ {"graph", {{"description", "Graph instance, e.g. \"0x06\" (Power)."}}},
				{"history_types", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "History type IDs, e.g. [\"0x4A11D354\"]."}}},
				{"months", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}}} }), Permission::Read,
			GameThread([](const json& arguments) { return GetHistory(arguments); }) });

		tools.push_back({ "get_utilities",
			"Power (production, consumption, demand, imports, plant counts and, with per_plant, every plant's output, load, efficiency and cost), garbage (produced, landfilled, recycled, incinerated, capacity) and the water fields of the advisor data.",
			Schema({ {"per_plant", {{"type", "boolean"}}} }), Permission::Read,
			GameThread([](const json& arguments) { return GetUtilities(arguments.value("per_plant", true)); }) });

		tools.push_back({ "get_services",
			"Education (coverage, EQ, grades), health (coverage, patients against capacity, life expectancy), police (stations, coverage, arrests), jails (capacity, inmates), crime and fire coverage. per_building=true lists schools and hospitals with capacity and radius, police and fire stations and jails.",
			Schema({ {"per_building", {{"type", "boolean"}}} }), Permission::Read,
			GameThread([](const json& arguments) { return GetServices(arguments.value("per_building", false)); }) });

		tools.push_back({ "query_cell",
			"Everything about one city cell: zone, lot and building (state, occupancy, population and capacity, power, road access, cost), land value, pollution, crime, police and fire coverage, power and congestion.",
			Schema({ {"x", {{"type", "integer"}, {"minimum", 0}}}, {"z", {{"type", "integer"}, {"minimum", 0}}} }, json::array({ "x", "z" })), Permission::Read,
			GameThread([](const json& arguments) { return QueryCell(arguments.at("x").get<int32_t>(), arguments.at("z").get<int32_t>()); }) });

		tools.push_back({ "get_map_stats",
			"Statistics of a city map: min, max, average, histogram and the worst cells. Maps: zones, land_value, crime, air_pollution, water_pollution, garbage, fire_protection, police_coverage, congestion, powered. developed_only=true counts only cells with a lot.",
			Schema({ {"map", {{"type", "string"}}}, {"top", {{"type", "integer"}, {"minimum", 0}, {"maximum", 50}}}, {"developed_only", {{"type", "boolean"}}} }, json::array({ "map" })), Permission::Read,
			GameThread([](const json& arguments)
			{
				return GetMapStats(Lower(arguments.at("map").get<std::string>()), std::clamp(arguments.value("top", 10), 0, 50), arguments.value("developed_only", false));
			}) });

		tools.push_back({ "list_lots",
			"Lots with a building: cells, zone, state, occupancy, population and capacity by type, wealth, land value, power and road access. Filters: zone (e.g. residential_high), state (occupied, vacant, constructing... or abandoned), powered (true/false), limit.",
			Schema({ {"zone", {{"type", "string"}}}, {"state", {{"type", "string"}}}, {"powered", {{"type", "boolean"}}},
				{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}} }), Permission::Read,
			GameThread([](const json& arguments) { return ListLots(arguments); }) });

		tools.push_back({ "list_ordinances",
			"Every ordinance with its ID, name, description, availability, on/off state and monthly income or cost.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return ListOrdinances(); }) });

		tools.push_back({ "get_advisors",
			"The advisors with their current messages (title, text, priority, mood), and the message shown in the popup.",
			Schema({ {"max_messages", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}} }), Permission::Read,
			GameThread([](const json& arguments) { return GetAdvisors(std::clamp(arguments.value("max_messages", 10), 1, 50)); }) });
	}
}
