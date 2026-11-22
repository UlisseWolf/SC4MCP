// SC4MCP - diagnostics ("why does it not grow?"), building and exemplar properties, and snapshots.
//
// diagnose_growth and diagnose_cell turn the game's own state (demand, zones, the vacate reason the game stores on
// every abandoned lot, development failures, power, road access, land value) into findings. get_building_properties
// lists the properties of a building as the game loaded them, after every plugin override. Snapshots keep a copy of
// the main city figures in memory and compare them later, returning only what changed.
#include "ToolsCommon.h"

#include "cIGZPersistResourceManager.h"
#include "cIGZString.h"
#include "cIGZUnknownList.h"
#include "cIGZVariant.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4BuildingDevelopmentSimulator.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4City.h"
#include "cISC4CrimeSimulator.h"
#include "cISC4Demand.h"
#include "cISC4DemandSimulator.h"
#include "cISC4LandValueSimulator.h"
#include "cISC4Lot.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISC4PollutionSimulator.h"
#include "cISC4PowerSimulator.h"
#include "cISC4SimGrid.h"
#include "cISC4Simulator.h"
#include "cISC4ZoneManager.h"
#include "cISCProperty.h"
#include "cISCPropertyHolder.h"
#include "cISCResExemplar.h"
#include "cGZPersistResourceKey.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	constexpr uint32_t kTotalsIndex = 0x20000;

	const char* VacateReasonName(int32_t reason)
	{
		switch (reason)
		{
		case 0: return "none";
		case 1: return "low_desirability";
		case 2: return "no_power";
		case 3: return "no_water";
		case 4: return "commute_time";
		case 5: return "kicked_out";
		case 6: return "low_demand";
		default: return "unknown";
		}
	}

	const char* VacateAdvice(int32_t reason)
	{
		switch (reason)
		{
		case 1: return "low desirability: check pollution, crime, land value, parks and services around these lots";
		case 2: return "no power: connect the lots to the grid or add power capacity";
		case 3: return "no water: extend the pipes or add water capacity";
		case 4: return "commute time too long: improve roads and transit, or bring jobs closer";
		case 5: return "occupants kicked out (usually by a rezoning or a cheat)";
		case 6: return "low demand: the demand for that wealth and type fell (taxes, caps, regional demand)";
		default: return "no reason recorded by the game";
		}
	}

	// The demand types that can grow in a zone.
	std::vector<std::pair<uint32_t, const char*>> DemandTypesForZone(int32_t zone)
	{
		switch (zone)
		{
		case 1: case 2: case 3: return { {0x1010, "R$"}, {0x1020, "R$$"}, {0x1030, "R$$$"} };
		case 4: case 5: case 6: return { {0x3110, "Cs$"}, {0x3120, "Cs$$"}, {0x3130, "Cs$$$"}, {0x3320, "Co$$"}, {0x3330, "Co$$$"} };
		case 7: return { {0x4100, "I-R"} };
		case 8: case 9: return { {0x4200, "I-D"}, {0x4300, "I-M"}, {0x4400, "I-HT"} };
		default: return {};
		}
	}

	float DemandValue(cISC4City* city, uint32_t id)
	{
		cISC4DemandSimulator* simulator = city->GetDemandSimulator();
		const cISC4Demand* demand = simulator ? simulator->GetDemand(id, kTotalsIndex) : nullptr;
		if (!demand && simulator) demand = simulator->GetDemand(id, 0);
		return demand ? demand->QueryDemandValue() : 0.0f;
	}

	// ------------------------------------------------------------------------------------------------- growth

	json DiagnoseGrowth()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		json findings = json::array();
		json out = json::object();

		// Demand
		static const std::pair<uint32_t, const char*> kTypes[] = {
			{0x1010, "R$"}, {0x1020, "R$$"}, {0x1030, "R$$$"}, {0x3110, "Cs$"}, {0x3120, "Cs$$"}, {0x3130, "Cs$$$"},
			{0x3320, "Co$$"}, {0x3330, "Co$$$"}, {0x4100, "I-R"}, {0x4200, "I-D"}, {0x4300, "I-M"}, {0x4400, "I-HT"} };
		json demand = json::object();
		std::vector<std::string> negative;
		for (const auto& [id, label] : kTypes)
		{
			const float value = DemandValue(city, id);
			demand[label] = value;
			if (value <= 0.0f) negative.push_back(label);
		}
		out["demand"] = demand;
		if (!negative.empty())
		{
			std::string list;
			for (const std::string& label : negative) list += (list.empty() ? "" : ", ") + label;
			findings.push_back("No demand for " + list + ": zones of these types will not grow (check taxes, caps and the regional demand with get_demand).");
		}

		// Zones
		cISC4ZoneManager* zones = city->GetZoneManager();
		if (zones)
		{
			json z = json::object();
			for (int32_t type = 1; type <= 9; ++type)
			{
				const auto zoneType = static_cast<cISC4ZoneManager::ZoneType>(type);
				const int32_t cells = zones->GetZoneCount(zoneType);
				if (!cells) continue;
				const int32_t empty = zones->GetUndevelopedTileCount(zoneType);
				z[ZoneName(type)] = json{ {"cells", cells}, {"undeveloped", empty}, {"undeveloped_percent", 100.0 * empty / cells} };
				if (cells >= 20 && empty * 2 > cells)
				{
					findings.push_back(std::string("More than half of the ") + ZoneName(type) + " zone is undeveloped (" + std::to_string(empty) + " of " + std::to_string(cells)
						+ " cells): check its demand, road access and power (diagnose_cell on an empty cell gives the reasons).");
				}
			}
			out["zones"] = z;
			out["abandoned_cells"] = zones->GetAbandonedTileCount();
		}

		// Lots: states, vacate reasons, power and road access
		cISC4LotManager* lots = city->GetLotManager();
		std::map<std::string, int> states, reasons;
		int unpowered = 0, noRoad = 0, growables = 0, lowOccupancy = 0;
		json examples = json::object();
		std::set<cISC4Lot*> seen;
		if (lots)
		{
			for (cISC4Occupant* occupant : AllBuildings(city))
			{
				cISC4Lot* lot = lots->GetOccupantLot(occupant);
				if (!lot || !seen.insert(lot).second) continue;
				const uint32_t state = static_cast<uint32_t>(lot->GetState());
				static const char* const kStates[] = { "created", "ready", "constructing", "occupied", "vacant", "destructing", "destroyed" };
				states[state < 7 ? kStates[state] : "unknown"]++;
				const int32_t zone = static_cast<int32_t>(lot->GetZoneType());
				const bool growable = zone >= 1 && zone <= 9;
				if (growable) ++growables;
				if (lot->GetState() == cISC4Lot::HabitationState::Vacant)
				{
					const int32_t reason = static_cast<int32_t>(lot->GetVacateReason());
					const std::string name = VacateReasonName(reason);
					reasons[name]++;
					if (!examples.contains(name))
					{
						SC4Rect<int32_t> rect{};
						if (lot->GetBoundingRect(rect)) examples[name] = json{ {"x", rect.topLeftX}, {"z", rect.topLeftY}, {"building", BuildingName(occupant)} };
					}
				}
				if (lot->GetNeedsPower() && !lot->GetHasPower()) ++unpowered;
				if (growable && !lot->HasRoadAccess()) ++noRoad;
				if (growable && lot->GetState() == cISC4Lot::HabitationState::Occupied && lot->GetOccupancy() < 0.5f) ++lowOccupancy;
			}
		}
		out["lots"] = json{ {"by_state", states}, {"growable", growables}, {"unpowered", unpowered}, {"growable_without_road_access", noRoad},
			{"growable_under_half_occupied", lowOccupancy} };
		out["abandoned_by_reason"] = reasons;
		out["abandoned_examples"] = examples;
		for (const auto& [name, count] : reasons)
		{
			int32_t reason = 0;
			for (int32_t r = 0; r <= 6; ++r) if (name == VacateReasonName(r)) reason = r;
			findings.push_back(std::to_string(count) + " abandoned lot(s) because of " + VacateAdvice(reason) + ".");
		}
		if (unpowered) findings.push_back(std::to_string(unpowered) + " lot(s) need power and have none.");
		if (noRoad) findings.push_back(std::to_string(noRoad) + " growable lot(s) have no road access.");

		// Development failures on empty zoned cells
		if (zones && lots)
		{
			struct Failure { int32_t count; int32_t x; int32_t z; };
			std::vector<Failure> failures;
			const int32_t sizeX = static_cast<int32_t>(city->CellCountX()), sizeZ = static_cast<int32_t>(city->CellCountZ());
			long long total = 0;
			for (int32_t x = 0; x < sizeX; ++x)
			{
				for (int32_t z = 0; z < sizeZ; ++z)
				{
					const int32_t zone = static_cast<int32_t>(zones->GetZoneType(x, z));
					if (zone < 1 || zone > 9 || lots->GetLot(x, z, false)) continue;
					const int32_t count = zones->GetDevelopmentFailureCount(x, z);
					if (count <= 0) continue;
					total += count;
					failures.push_back({ count, x, z });
				}
			}
			std::sort(failures.begin(), failures.end(), [](const Failure& a, const Failure& b) { return a.count > b.count; });
			json worst = json::array();
			for (size_t i = 0; i < failures.size() && i < 10; ++i) worst.push_back(json{ {"x", failures[i].x}, {"z", failures[i].z}, {"failures", failures[i].count} });
			out["development_failures"] = json{ {"total", total}, {"cells", failures.size()}, {"worst_cells", worst} };
			if (!failures.empty()) findings.push_back("The developer failed to build on " + std::to_string(failures.size()) + " empty zoned cell(s); the worst are listed in development_failures (diagnose_cell explains one).");
		}

		// Power
		if (cISC4PowerSimulator* power = city->GetPowerSimulator())
		{
			const int32_t production = power->GetMonthlyProduction(), consumption = power->GetMonthlyConsumption();
			out["power"] = json{ {"production", production}, {"consumption", consumption}, {"unpowered_consumers", power->GetUnpoweredConsumerCount()} };
			if (production > 0 && consumption * 100 >= production * 95) findings.push_back("Power plants run at " + std::to_string(consumption * 100 / production) + "% of their output: add capacity before the city grows.");
			if (production == 0 && power->GetUnpoweredConsumerCount() > 0) findings.push_back("The city has no power production.");
		}

		// Taxes against the neutral rate
		GameTable table;
		cISC4DemandSimulator* demandSimulator = city->GetDemandSimulator();
		if (demandSimulator && ReadGameTable(city, table))
		{
			const float neutral = demandSimulator->GetNeutralTaxRate();
			json high = json::array();
			for (const auto& [name, value] : table.numbers)
			{
				if (name.rfind("g_tax_rate_", 0) == 0 && value > neutral + 0.05) high.push_back(json{ {"tax", name.substr(11)}, {"rate", value} });
			}
			out["taxes_above_neutral"] = json{ {"neutral_rate", neutral}, {"rates", high} };
			if (!high.empty()) findings.push_back(std::to_string(high.size()) + " tax rate(s) are above the neutral rate (" + std::to_string(neutral).substr(0, 4) + "%), which lowers the demand of those types.");
		}

		if (findings.empty()) findings.push_back("No obvious obstacle to growth found.");
		out["findings"] = findings;
		out["note"] = "Desirability itself is not exposed by the game's interfaces; abandoned_by_reason and the cells' pollution, crime and land value stand in for it.";
		return out;
	}

	// ------------------------------------------------------------------------------------------------- one cell

	json DiagnoseCell(int32_t x, int32_t z)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		if (!InCity(city, x, z)) return Error("Cell outside the city.");
		json reasons = json::array();
		json out{ {"x", x}, {"z", z} };
		cISC4ZoneManager* zones = city->GetZoneManager();
		const int32_t zone = zones ? static_cast<int32_t>(zones->GetZoneType(x, z)) : 0;
		out["zone"] = ZoneName(zone);
		if (zone == 0) reasons.push_back("The cell is not zoned.");

		cISC4Lot* lot = nullptr;
		cISC4Occupant* building = BuildingAt(city, x, z, &lot);
		if (lot)
		{
			out["building"] = building ? BuildingName(building) : "";
			const uint32_t state = static_cast<uint32_t>(lot->GetState());
			static const char* const kStates[] = { "created", "ready", "constructing", "occupied", "vacant", "destructing", "destroyed" };
			out["state"] = state < 7 ? kStates[state] : "unknown";
			out["occupancy_percent"] = lot->GetOccupancy() * 100.0f;
			if (lot->GetState() == cISC4Lot::HabitationState::Vacant)
			{
				const int32_t reason = static_cast<int32_t>(lot->GetVacateReason());
				out["vacate_reason"] = VacateReasonName(reason);
				reasons.push_back(std::string("Abandoned: ") + VacateAdvice(reason) + ".");
			}
			if (lot->GetNeedsPower() && !lot->GetHasPower()) reasons.push_back("The lot needs power and has none.");
			if (!lot->HasRoadAccess() && zone >= 1 && zone <= 9) reasons.push_back("The lot has no road access.");
			if (lot->GetState() == cISC4Lot::HabitationState::Occupied && lot->GetOccupancy() < 0.5f) reasons.push_back("The building is less than half occupied: demand for its type is weak here.");
		}
		else if (zone >= 1 && zone <= 9 && zones)
		{
			const int32_t failures = zones->GetDevelopmentFailureCount(x, z);
			out["development_failures"] = failures;
			if (failures > 0) reasons.push_back("The developer tried to build here " + std::to_string(failures) + " time(s) and failed (no building fitted the demand, the land value or the lot size).");
			cISC4PowerSimulator* power = city->GetPowerSimulator();
			if (power && !power->IsPowered(x, z)) reasons.push_back("The cell has no power: buildings will not grow without it.");
		}

		// Demand of the types that grow in this zone
		json demand = json::object();
		std::vector<std::string> none;
		for (const auto& [id, label] : DemandTypesForZone(zone))
		{
			const float value = DemandValue(city, id);
			demand[label] = value;
			if (value <= 0.0f) none.push_back(label);
		}
		if (!demand.empty()) out["zone_demand"] = demand;
		if (!none.empty() && none.size() == DemandTypesForZone(zone).size()) reasons.push_back("There is no demand for any type that grows in this zone.");

		if (cISC4LandValueSimulator* landValue = city->GetLandValueSimulator())
		{
			const uint32_t type = static_cast<uint32_t>(landValue->GetLandValueType(x, z));
			out["land_value"] = landValue->GetLandValue(x, z);
			static const char* const types[] = { "", "low", "medium", "high" };
			out["land_value_type"] = type <= 3 ? types[type] : "unknown";
			out["note_land_value"] = "Low land value lets only $ buildings grow, high land value favours $$$.";
		}
		if (cISC4PollutionSimulator* pollution = city->GetPollutionSimulator())
		{
			int32_t air = 0, water = 0, garbage = 0;
			pollution->GetAirValue(x, z, air);
			pollution->GetWaterValue(x, z, water);
			pollution->GetGarbageValue(x, z, garbage);
			out["pollution"] = json{ {"air", air}, {"water", water}, {"garbage", garbage}, {"radioactive", pollution->IsRadioactive(x, z)} };
			if (pollution->IsRadioactive(x, z)) reasons.push_back("The cell is radioactive.");
			if (pollution->IsAirPolluted(x, z) && zone >= 1 && zone <= 6) reasons.push_back("Air pollution is high here, which lowers residential and commercial desirability.");
			if (pollution->IsGarbageAccumulated(x, z)) reasons.push_back("Garbage accumulates here.");
		}
		if (cISC4CrimeSimulator* crime = city->GetCrimeSimulator())
		{
			if (cISC4SimGrid<uint8_t>* grid = crime->GetCrimeGrid())
			{
				const int value = grid->GetCellValue(x, z);
				out["crime"] = value;
				if (value >= 128) reasons.push_back("Crime is high here.");
			}
		}
		if (reasons.empty()) reasons.push_back("No obstacle found on this cell.");
		out["reasons"] = reasons;
		return out;
	}

	// ------------------------------------------------------------------------------------------------- properties

	// Common property names; the full list is in new_properties.xml of the modding tools.
	const char* PropertyName(uint32_t id)
	{
		switch (id)
		{
		case 0x00000010: return "Exemplar Type";
		case 0x00000020: return "Exemplar Name";
		case 0x00000021: return "Exemplar ID";
		case 0x27812810: return "Occupant Size";
		case 0x27812834: return "Capacity Satisfied";
		case 0x27812840: return "Demand Satisfied";
		case 0x27812841: return "Demand Created";
		case 0x49CAC341: return "Plop Cost";
		case 0x4AA60EBC: return "Catalog Capacity";
		case 0x8A416A99: return "User Visible Name Key";
		case 0xAA1DD396: return "Occupant Groups";
		case 0xEA54D286: return "Budget Item Cost";
		case 0x8922041B: return "Students";
		case 0x69220415: return "Patients";
		case 0x691B42B3: return "School Coverage Radius";
		case 0x091B42A2: return "Hospital Coverage Radius";
		case 0x08F55DC2: return "Fire Station Coverage Radius";
		case 0x48D71ED2: return "Police Station Coverage Radius";
		default: return nullptr;
		}
	}

	json VariantValue(const cIGZVariant* value)
	{
		if (!value) return nullptr;
		const uint16_t type = value->GetType();
		const bool isArray = (type & cIGZVariant::TypeArray) != 0;
		const uint16_t base = type & 0x00FF;
		const uint32_t count = isArray ? std::min<uint32_t>(value->GetCount(), 4096) : 1;
		json values = json::array();
		auto hexValue = [](uint32_t v) { return Hex(v); };
		switch (base)
		{
		case cIGZVariant::Bool: { const bool* d = value->RefBool(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Uint8: { const uint8_t* d = value->RefUint8(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Sint8: { const int8_t* d = value->RefSint8(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Uint16: { const uint16_t* d = value->RefUint16(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Sint16: { const int16_t* d = value->RefSint16(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Uint32: { const uint32_t* d = value->RefUint32(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(hexValue(d[i])); break; }
		case cIGZVariant::Sint32: { const int32_t* d = value->RefSint32(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Uint64: { const uint64_t* d = value->RefUint64(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Sint64: { const int64_t* d = value->RefSint64(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Float32: { const float* d = value->RefFloat32(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Float64: { const double* d = value->RefFloat64(); for (uint32_t i = 0; d && i < count; ++i) values.push_back(d[i]); break; }
		case cIGZVariant::Char:
		{
			const char* d = value->RefChar();
			return d ? json(std::string(d, d + value->GetCount())) : json(nullptr);
		}
		default: return "(unsupported type " + Hex(type) + ")";
		}
		if (!isArray && values.size() == 1) return values[0];
		return values;
	}

	json PropertyList(const cISCPropertyHolder* holder)
	{
		json list = json::array();
		cIGZUnknownList* properties = nullptr;
		if (!holder || !holder->GetPropertyList(&properties) || !properties) return list;
		const size_t size = std::min<size_t>(properties->GetSize(), 2000);
		for (size_t i = 0; i < size; ++i)
		{
			cISCProperty* property = nullptr;
			if (!properties->GetElement(static_cast<uint32_t>(i), GZIID_cISCProperty, reinterpret_cast<void**>(&property)) || !property) continue;
			const uint32_t id = property->GetPropertyID();
			json entry{ {"id", Hex(id)}, {"value", VariantValue(property->GetPropertyValue())} };
			if (const char* name = PropertyName(id)) entry["name"] = name;
			list.push_back(entry);
			property->Release();
		}
		properties->Release();
		std::sort(list.begin(), list.end(), [](const json& a, const json& b) { return a["id"].get<std::string>() < b["id"].get<std::string>(); });
		return list;
	}

	json ExemplarProperties(const cGZPersistResourceKey& key)
	{
		cIGZPersistResourceManagerPtr resources;
		cRZAutoRefCount<cISCResExemplar> exemplar;
		if (!resources || !resources->GetResource(key, GZIID_cISCResExemplar, exemplar.AsPPVoid(), 0, nullptr) || !exemplar)
		{
			return Error("No exemplar " + Hex(key.type) + " / " + Hex(key.group) + " / " + Hex(key.instance) + ".");
		}
		return json{ {"key", json{ {"type", Hex(key.type)}, {"group", Hex(key.group)}, {"instance", Hex(key.instance)} }},
			{"properties", PropertyList(exemplar->AsISCPropertyHolder())} };
	}

	json GetBuildingProperties(const json& arguments)
	{
		cISC4City* city = City();
		if (arguments.contains("type") && arguments.contains("group") && arguments.contains("instance"))
		{
			return ExemplarProperties(cGZPersistResourceKey(ParseId(arguments["type"]), ParseId(arguments["group"]), ParseId(arguments["instance"])));
		}
		if (!city) return NoCity();
		cISC4BuildingDevelopmentSimulator* development = city->GetBuildingDevelopmentSimulator();
		uint32_t buildingType = 0;
		json out = json::object();
		if (arguments.contains("x") && arguments.contains("z"))
		{
			cISC4Occupant* building = BuildingAt(city, arguments["x"].get<int32_t>(), arguments["z"].get<int32_t>());
			if (!building) return Error("No building on that cell.");
			buildingType = BuildingType(building);
			out["building"] = BuildingName(building);
			out["cells"] = BuildingCells(building);
			out["in_city_properties"] = PropertyList(building->AsPropertyHolder());
			const BuildingCost cost = CostOf(city->GetBudgetSimulator(), building);
			if (!cost.items.empty()) out["monthly_cost_in_game"] = cost.monthly;
		}
		else if (arguments.contains("exemplar"))
		{
			buildingType = ParseId(arguments["exemplar"]);
		}
		else
		{
			return Error("Give x and z (a building in the city), exemplar (a building exemplar ID) or type, group and instance (any exemplar).");
		}
		out["exemplar"] = Hex(buildingType);
		cGZPersistResourceKey key;
		if (development && development->GetBuildingKeyFromType(buildingType, key))
		{
			out["exemplar_file"] = ExemplarProperties(key);
		}
		else
		{
			out["exemplar_file"] = Error("The game has no exemplar for this building ID.");
		}
		out["note"] = "in_city_properties are the values of this building as the game holds them; exemplar_file has the properties of the exemplar the game loaded (after every plugin override). Inherited cohort properties are not repeated.";
		return out;
	}

	// ------------------------------------------------------------------------------------------------- snapshots

	struct Snapshot
	{
		json data;
		std::string created;
	};
	std::mutex snapshotMutex;
	std::map<std::string, Snapshot> snapshots;
	constexpr size_t kMaxSnapshots = 10;

	// Flattens the numbers of a JSON value into path -> value. Array elements are keyed by their id, line, name or
	// type field when they have one, so departments and lines match between snapshots.
	void Flatten(const json& value, const std::string& path, std::map<std::string, double>& out)
	{
		if (value.is_number())
		{
			out[path] = value.get<double>();
		}
		else if (value.is_boolean())
		{
			out[path] = value.get<bool>() ? 1.0 : 0.0;
		}
		else if (value.is_object())
		{
			for (auto it = value.begin(); it != value.end(); ++it)
			{
				if (it.key() == "day_number") continue;
				Flatten(it.value(), path.empty() ? it.key() : path + "." + it.key(), out);
			}
		}
		else if (value.is_array())
		{
			for (size_t i = 0; i < value.size(); ++i)
			{
				const json& element = value[i];
				std::string key = std::to_string(i);
				for (const char* field : { "id", "line", "name", "type", "history_type" })
				{
					if (element.is_object() && element.contains(field) && element[field].is_string()) { key = element[field].get<std::string>(); break; }
				}
				Flatten(element, path + "[" + key + "]", out);
			}
		}
	}

	json CollectSnapshot(bool& isError)
	{
		json data = json::object();
		const std::pair<const char*, json> parts[] = {
			{ "get_city_status", json::object() }, { "get_budget", json::object() }, { "get_demand", json::object() },
			{ "get_taxes", json::object() }, { "get_services", json::object() }, { "get_utilities", json{ {"per_plant", false} } } };
		for (const auto& [tool, arguments] : parts)
		{
			bool partError = false;
			json part = CallTool(tool, arguments, partError);
			if (partError)
			{
				if (std::string(tool) == "get_city_status") { isError = true; return part; }
				continue;
			}
			part.erase("note");
			part.erase("buildings_note");
			data[tool] = part;
		}
		return data;
	}

	std::string Now()
	{
		const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		char text[32];
		std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
		return text;
	}

	json SnapshotSave(const json& arguments, bool& isError)
	{
		const std::string name = arguments.value("name", std::string("default"));
		json data = CollectSnapshot(isError);
		if (isError) return data;
		std::lock_guard<std::mutex> lock(snapshotMutex);
		if (snapshots.size() >= kMaxSnapshots && !snapshots.count(name)) return Error("At most 10 snapshots: delete one with snapshot_delete.");
		snapshots[name] = { data, Now() };
		std::map<std::string, double> values;
		Flatten(data, "", values);
		return json{ {"saved", name}, {"game_date", data["get_city_status"].value("date", json::object()).value("text", std::string())},
			{"values", values.size()}, {"note", "Use snapshot_compare with this name after changes or wait_months."} };
	}

	json SnapshotCompare(const json& arguments, bool& isError)
	{
		const std::string name = arguments.value("name", std::string("default"));
		const double minPercent = std::max(0.0, arguments.value("min_percent", 0.0));
		const size_t limit = static_cast<size_t>(std::clamp(arguments.value("limit", 150), 1, 2000));
		Snapshot before;
		{
			std::lock_guard<std::mutex> lock(snapshotMutex);
			const auto found = snapshots.find(name);
			if (found == snapshots.end()) { isError = true; return Error("No snapshot named " + name + " (snapshot_list lists them)."); }
			before = found->second;
		}
		json now = CollectSnapshot(isError);
		if (isError) return now;
		std::map<std::string, double> a, b;
		Flatten(before.data, "", a);
		Flatten(now, "", b);
		struct Change { std::string path; double before; double after; double percent; };
		std::vector<Change> changes;
		for (const auto& [path, after] : b)
		{
			const auto found = a.find(path);
			if (found == a.end()) continue;
			const double old = found->second;
			if (old == after) continue;
			const double percent = old != 0.0 ? (after - old) * 100.0 / std::fabs(old) : 100.0;
			if (std::fabs(percent) < minPercent) continue;
			changes.push_back({ path, old, after, percent });
		}
		std::sort(changes.begin(), changes.end(), [](const Change& x, const Change& y) { return std::fabs(x.percent) > std::fabs(y.percent); });
		json list = json::array();
		for (size_t i = 0; i < changes.size() && i < limit; ++i)
		{
			list.push_back(json{ {"value", changes[i].path}, {"before", changes[i].before}, {"after", changes[i].after},
				{"change", changes[i].after - changes[i].before}, {"percent", std::round(changes[i].percent * 10.0) / 10.0} });
		}
		size_t added = 0, removed = 0;
		for (const auto& [path, value] : b) if (!a.count(path)) ++added;
		for (const auto& [path, value] : a) if (!b.count(path)) ++removed;
		auto dateOf = [](const json& data) { return data.value("get_city_status", json::object()).value("date", json::object()).value("text", std::string()); };
		return json{ {"snapshot", name}, {"from_date", dateOf(before.data)}, {"to_date", dateOf(now)}, {"changed_values", changes.size()},
			{"returned", list.size()}, {"new_values", added}, {"vanished_values", removed}, {"changes", list},
			{"note", "Sorted by the size of the change in percent. new_values and vanished_values count values that exist in only one of the two (new buildings, departments...)."} };
	}

	json SnapshotList()
	{
		std::lock_guard<std::mutex> lock(snapshotMutex);
		json list = json::array();
		for (const auto& [name, snapshot] : snapshots)
		{
			list.push_back(json{ {"name", name}, {"saved_at", snapshot.created},
				{"game_date", snapshot.data.value("get_city_status", json::object()).value("date", json::object()).value("text", std::string())} });
		}
		return json{ {"snapshots", list}, {"note", "Snapshots live in memory and are lost when the game closes."} };
	}
}

namespace ToolsCommon
{
	void AddDiagnosticTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "diagnose_growth",
			"Why the city does or does not grow: demand per type, undeveloped zones, abandoned lots by the reason the game recorded (low desirability, no power, no water, commute time, low demand), unpowered lots and lots without road access, development failures, power margin and taxes above the neutral rate, with plain findings.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return DiagnoseGrowth(); }) });

		tools.push_back({ "diagnose_cell",
			"Why one cell does not grow or was abandoned: zone, building state and vacate reason, power, road access, development failures, the demand of the types that grow there, land value, pollution and crime, with plain reasons.",
			Schema({ {"x", {{"type", "integer"}, {"minimum", 0}}}, {"z", {{"type", "integer"}, {"minimum", 0}}} }, json::array({ "x", "z" })), Permission::Read,
			GameThread([](const json& arguments) { return DiagnoseCell(arguments.at("x").get<int32_t>(), arguments.at("z").get<int32_t>()); }) });

		tools.push_back({ "get_building_properties",
			"Properties of a building as the game loaded them, after every plugin override: give x and z (a building in the city: its in-city values and its exemplar), exemplar (a building exemplar ID) or type, group and instance (any exemplar). Values of Uint32 properties are shown in hex.",
			Schema({ {"x", {{"type", "integer"}}}, {"z", {{"type", "integer"}}}, {"exemplar", {{"type", "string"}}},
				{"type", {{"type", "string"}}}, {"group", {{"type", "string"}}}, {"instance", {{"type", "string"}}} }), Permission::Read,
			GameThread([](const json& arguments) { return GetBuildingProperties(arguments); }) });

		tools.push_back({ "snapshot_save",
			"Saves a snapshot of the main city figures (status, budget, demand, taxes, services, utilities) under a name (default \"default\"), to compare later with snapshot_compare.",
			Schema({ {"name", {{"type", "string"}}} }), Permission::Read,
			[](const json& arguments, bool& isError, const Tools::Context&) { return SnapshotSave(arguments, isError); } });

		tools.push_back({ "snapshot_compare",
			"Compares the city now with a saved snapshot and returns only the values that changed (before, after, change, percent), largest changes first. min_percent hides small changes.",
			Schema({ {"name", {{"type", "string"}}}, {"min_percent", {{"type", "number"}, {"minimum", 0}}}, {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2000}}} }), Permission::Read,
			[](const json& arguments, bool& isError, const Tools::Context&) { return SnapshotCompare(arguments, isError); } });

		tools.push_back({ "snapshot_list",
			"The saved snapshots with the time and game date they were taken.",
			Schema(json::object()), Permission::Read,
			[](const json&, bool&, const Tools::Context&) { return SnapshotList(); } });

		tools.push_back({ "snapshot_delete",
			"Deletes a saved snapshot.",
			Schema({ {"name", {{"type", "string"}}} }, json::array({ "name" })), Permission::Read,
			[](const json& arguments, bool& isError, const Tools::Context&) -> json
			{
				std::lock_guard<std::mutex> lock(snapshotMutex);
				const std::string name = arguments.at("name").get<std::string>();
				if (!snapshots.erase(name)) { isError = true; return Error("No snapshot named " + name + "."); }
				return json{ {"deleted", name} };
			} });
	}
}
