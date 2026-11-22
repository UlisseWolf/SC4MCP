// SC4MCP - planning and modding tools: where to build services, CSV export, region, building audit, resources,
// plugins (files, DLLs, overrides between plugins), cheats, and scenarios that chain tools with checks.
#include "ToolsCommon.h"
#include "McpLog.h"

#include "cIGZCheatCodeManager.h"
#include "cIGZPersistResourceManager.h"
#include "cIGZString.h"
#include "cISC4App.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4BuildingDevelopmentSimulator.h"
#include "cISC4City.h"
#include "cISC4Lot.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISC4PowerSimulator.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cISC4ZoneDeveloper.h"
#include "cISC4ZoneManager.h"
#include "cISCPropertyHolder.h"
#include "cISCResExemplar.h"
#include "cISCResExemplarCohort.h"
#include "cGZPersistResourceKey.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"
#include "SC4List.h"
#include "SC4Rect.h"
#include "SC4String.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

using nlohmann::json;
using namespace ToolsCommon;
namespace fs = std::filesystem;

namespace
{
	constexpr uint32_t kPropStudents = 0x8922041B;
	constexpr uint32_t kPropPatients = 0x69220415;
	constexpr uint32_t kPropSchoolRadius = 0x691B42B3;
	constexpr uint32_t kPropHospitalRadius = 0x091B42A2;
	constexpr uint32_t kPropFireRadius = 0x08F55DC2;
	constexpr uint32_t kPropPoliceRadius = 0x48D71ED2;
	constexpr uint32_t kPropPlopCost = 0x49CAC341;
	constexpr double kMetresPerCell = 16.0;

	bool Prop(cISC4Occupant* occupant, uint32_t id, uint32_t& value)
	{
		const cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
		return holder && holder->GetProperty(id, value);
	}

	// ------------------------------------------------------------------------------------------------- where to build

	struct Family { const char* name; uint32_t radiusProp; uint32_t capacityProp; double defaultRadius; bool everyone; };
	const Family kFamilies[] = {
		{ "school", kPropSchoolRadius, kPropStudents, 600, false },
		{ "hospital", kPropHospitalRadius, kPropPatients, 600, false },
		{ "fire", kPropFireRadius, 0, 450, true },
		{ "police", kPropPoliceRadius, 0, 450, true } };

	json SuggestLocation(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		const std::string service = Lower(arguments.at("service").get<std::string>());
		const Family* family = nullptr;
		for (const Family& candidate : kFamilies) if (service == candidate.name) family = &candidate;
		if (!family) return Error("service must be school, hospital, fire or police.");
		const int count = std::clamp(arguments.value("count", 3), 1, 10);
		const int32_t sizeX = static_cast<int32_t>(city->CellCountX()), sizeZ = static_cast<int32_t>(city->CellCountZ());
		cISC4LotManager* lots = city->GetLotManager();
		if (!lots) return Error("Lot manager unavailable.");

		// People per cell: residents of R lots (schools, hospitals) or residents and workers of every lot (fire, police).
		std::vector<double> people(static_cast<size_t>(sizeX) * sizeZ, 0.0);
		std::vector<double> existingRadii;
		struct Station { double x, z, radius; };
		std::vector<Station> stations;
		static const uint32_t kResidential[] = { 0x1010, 0x1020, 0x1030 };
		static const uint32_t kJobs[] = { 0x3110, 0x3120, 0x3130, 0x3320, 0x3330, 0x4100, 0x4200, 0x4300, 0x4400 };
		std::set<cISC4Lot*> seen;
		for (cISC4Occupant* occupant : AllBuildings(city))
		{
			uint32_t radius = 0;
			if (Prop(occupant, family->radiusProp, radius) && radius > 0)
			{
				SC4Rect<long> cells{};
				if (occupant->GetBoundingCityCells(cells))
				{
					stations.push_back({ (cells.topLeftX + cells.bottomRightX) / 2.0, (cells.topLeftY + cells.bottomRightY) / 2.0, radius / kMetresPerCell });
					existingRadii.push_back(radius);
				}
			}
			cISC4Lot* lot = lots->GetOccupantLot(occupant);
			if (!lot || !seen.insert(lot).second) continue;
			double total = 0;
			for (uint32_t type : kResidential) total += lot->GetPopulation(static_cast<cISC4BuildingDevelopmentSimulator::DeveloperType>(type));
			if (family->everyone) for (uint32_t type : kJobs) total += lot->GetPopulation(static_cast<cISC4BuildingDevelopmentSimulator::DeveloperType>(type));
			if (total <= 0) continue;
			SC4Rect<int32_t> rect{};
			if (!lot->GetBoundingRect(rect)) continue;
			const int area = std::max(1, (rect.bottomRightX - rect.topLeftX + 1) * (rect.bottomRightY - rect.topLeftY + 1));
			for (int32_t x = std::max(0, rect.topLeftX); x <= std::min(sizeX - 1, rect.bottomRightX); ++x)
				for (int32_t z = std::max(0, rect.topLeftY); z <= std::min(sizeZ - 1, rect.bottomRightY); ++z)
					people[static_cast<size_t>(z) * sizeX + x] += total / area;
		}

		double radiusMetres = arguments.value("radius_m", 0.0);
		if (radiusMetres <= 0)
		{
			if (!existingRadii.empty()) { std::sort(existingRadii.begin(), existingRadii.end()); radiusMetres = existingRadii[existingRadii.size() / 2]; }
			else radiusMetres = family->defaultRadius;
		}
		const double radius = std::max(2.0, radiusMetres / kMetresPerCell);

		// Uncovered people.
		double totalPeople = 0, covered = 0;
		std::vector<double> uncovered = people;
		for (int32_t z = 0; z < sizeZ; ++z)
		{
			for (int32_t x = 0; x < sizeX; ++x)
			{
				double& value = uncovered[static_cast<size_t>(z) * sizeX + x];
				totalPeople += value;
				for (const Station& station : stations)
				{
					if ((x - station.x) * (x - station.x) + (z - station.z) * (z - station.z) <= station.radius * station.radius) { covered += value; value = 0; break; }
				}
			}
		}

		cISC4ZoneDeveloper* zoneDeveloper = city->GetZoneDeveloper();
		cISC4ZoneManager* zones = city->GetZoneManager();
		auto buildable = [&](int32_t x, int32_t z)
		{
			if (lots->GetLot(x, z, false)) return false;
			if (zoneDeveloper && zoneDeveloper->IsCellWater(x, z)) return false;
			return true;
		};
		const int r = static_cast<int>(std::ceil(radius));
		const int step = std::max(2, r / 3);
		json suggestions = json::array();
		for (int n = 0; n < count; ++n)
		{
			double best = 0;
			int32_t bestX = -1, bestZ = -1;
			for (int32_t cz = 0; cz < sizeZ; cz += step)
			{
				for (int32_t cx = 0; cx < sizeX; cx += step)
				{
					double sum = 0;
					for (int32_t z = std::max(0, cz - r); z <= std::min(sizeZ - 1, cz + r); ++z)
						for (int32_t x = std::max(0, cx - r); x <= std::min(sizeX - 1, cx + r); ++x)
							if ((x - cx) * (x - cx) + (z - cz) * (z - cz) <= radius * radius) sum += uncovered[static_cast<size_t>(z) * sizeX + x];
					if (sum > best) { best = sum; bestX = cx; bestZ = cz; }
				}
			}
			if (bestX < 0 || best < 1) break;
			// The nearest buildable cell (no lot, not water), preferring unzoned land.
			int32_t siteX = -1, siteZ = -1;
			double siteDistance = 1e9;
			for (int32_t z = std::max(0, bestZ - r / 2); z <= std::min(sizeZ - 1, bestZ + r / 2); ++z)
			{
				for (int32_t x = std::max(0, bestX - r / 2); x <= std::min(sizeX - 1, bestX + r / 2); ++x)
				{
					if (!buildable(x, z)) continue;
					double distance = std::hypot(x - bestX, z - bestZ);
					if (zones && zones->GetZoneType(x, z) != cISC4ZoneManager::ZoneType::None) distance += 3;
					if (distance < siteDistance) { siteDistance = distance; siteX = x; siteZ = z; }
				}
			}
			for (int32_t z = std::max(0, bestZ - r); z <= std::min(sizeZ - 1, bestZ + r); ++z)
				for (int32_t x = std::max(0, bestX - r); x <= std::min(sizeX - 1, bestX + r); ++x)
					if ((x - bestX) * (x - bestX) + (z - bestZ) * (z - bestZ) <= radius * radius) uncovered[static_cast<size_t>(z) * sizeX + x] = 0;
			json suggestion{ {"center", json{ {"x", bestX}, {"z", bestZ} }}, {"people_newly_covered", std::round(best)} };
			suggestion["free_cell_nearby"] = siteX >= 0 ? json{ {"x", siteX}, {"z", siteZ} } : json(nullptr);
			if (siteX < 0) suggestion["note"] = "No free cell near the centre: the area is built up; rezone or demolish to make room.";
			suggestions.push_back(suggestion);
		}
		return json{ {"service", service}, {"radius_m", radiusMetres}, {"existing_buildings", stations.size()},
			{"people", std::round(totalPeople)}, {"people_covered_now", std::round(covered)},
			{"coverage_percent_now", totalPeople > 0 ? std::round(1000.0 * covered / totalPeople) / 10.0 : 0.0},
			{"suggestions", suggestions},
			{"note", std::string("People are the ") + (family->everyone ? "residents and workers" : "residents") + " of the lots; coverage uses the radius property of the buildings (16 m per cell). Check the slope and the road access of the free cell before plopping. render_map highlight can show the existing buildings."} };
	}

	// ------------------------------------------------------------------------------------------------- CSV export

	std::string CsvField(const json& value)
	{
		std::string text = value.is_string() ? value.get<std::string>() : value.is_null() ? std::string() : value.dump();
		if (text.find_first_of(",\"\n;") != std::string::npos)
		{
			std::string quoted = "\"";
			for (char c : text) { if (c == '"') quoted += "\"\""; else quoted += c; }
			return quoted + "\"";
		}
		return text;
	}

	// Flattens an object one level (nested objects become prefix.key) for a CSV row.
	void FlattenRow(const json& object, const std::string& prefix, std::map<std::string, json>& row)
	{
		for (auto it = object.begin(); it != object.end(); ++it)
		{
			const std::string key = prefix.empty() ? it.key() : prefix + "." + it.key();
			if (it.value().is_object()) FlattenRow(it.value(), key, row);
			else if (!it.value().is_array()) row[key] = it.value();
		}
	}

	json WriteCsv(const std::string& what, const json& rows, const std::vector<std::string>& preferred)
	{
		std::vector<std::map<std::string, json>> flat;
		std::vector<std::string> found;
		std::set<std::string> known;
		for (const json& row : rows)
		{
			std::map<std::string, json> values;
			if (row.is_object()) FlattenRow(row, "", values);
			for (const auto& [key, value] : values) if (known.insert(key).second) found.push_back(key);
			flat.push_back(std::move(values));
		}
		// The preferred columns first (those present), then the others in the order they appear.
		std::vector<std::string> columns;
		for (const std::string& column : preferred) if (known.count(column)) columns.push_back(column);
		for (const std::string& column : found) if (std::find(columns.begin(), columns.end(), column) == columns.end()) columns.push_back(column);
		const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
		const fs::path path = fs::path(DataFolder("SC4MCP_exports")) / (what + "_" + stamp + ".csv");
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file) return Error("Cannot write " + path.string());
		file << "\xEF\xBB\xBF"; // UTF-8 BOM, so Excel opens accents correctly
		for (size_t i = 0; i < columns.size(); ++i) file << (i ? "," : "") << CsvField(columns[i]);
		file << "\r\n";
		for (const auto& values : flat)
		{
			for (size_t i = 0; i < columns.size(); ++i)
			{
				const auto found = values.find(columns[i]);
				file << (i ? "," : "") << (found != values.end() ? CsvField(found->second) : std::string());
			}
			file << "\r\n";
		}
		return json{ {"file", path.string()}, {"rows", flat.size()}, {"columns", columns.size()} };
	}

	json MapCells(const std::string& name)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		const MapReader* map = FindMapReader(name);
		if (!map) return Error("Unknown map.");
		json rows = json::array();
		const int32_t sizeX = static_cast<int32_t>(city->CellCountX()), sizeZ = static_cast<int32_t>(city->CellCountZ());
		for (int32_t z = 0; z < sizeZ; ++z)
			for (int32_t x = 0; x < sizeX; ++x)
			{
				double value = 0;
				if (map->read(city, x, z, value)) rows.push_back(json{ {"x", x}, {"z", z}, {"value", value} });
			}
		return json{ {"rows", rows} };
	}

	json ExportCsv(const json& arguments, bool& isError)
	{
		const std::string what = Lower(arguments.at("what").get<std::string>());
		json args = arguments.value("arguments", json::object());
		if (what == "buildings")
		{
			args["limit"] = 2000;
			json data = CallTool("list_buildings", args, isError);
			return isError ? data : WriteCsv(what, data["buildings"], { "name", "exemplar", "cells.x1", "cells.z1", "cells.x2", "cells.z2", "monthly_cost" });
		}
		if (what == "lots")
		{
			args["limit"] = 2000;
			json data = CallTool("list_lots", args, isError);
			return isError ? data : WriteCsv(what, data["lots"], { "building", "exemplar", "zone", "state", "cells.x1", "cells.z1", "occupancy_percent", "land_value" });
		}
		if (what == "budget")
		{
			json data = CallTool("get_budget", json::object(), isError);
			if (isError) return data;
			json rows = json::array();
			for (const json& department : data["departments"])
				for (const json& line : department["lines"])
				{
					json row = line;
					row["department"] = department.value("id", std::string());
					row["department_name"] = department.value("name", std::string());
					row["funding_percent"] = department.value("funding_percent", 0.0);
					rows.push_back(row);
				}
			return WriteCsv(what, rows, { "department", "department_name", "funding_percent", "line" });
		}
		if (what == "history")
		{
			json data = CallTool("get_history", args, isError);
			if (isError) return data;
			json rows = json::array();
			const json& months = data["months"];
			for (size_t i = 0; i < months.size(); ++i)
			{
				json row{ {"month", months[i]} };
				for (const json& series : data["series"])
				{
					const std::string label = series.value("label", std::string()).empty() ? series.value("history_type", std::string()) : series["label"].get<std::string>();
					row[label] = series["values"][i];
				}
				rows.push_back(row);
			}
			return WriteCsv(what, rows, { "month" });
		}
		if (what == "map")
		{
			const std::string name = Lower(args.value("map", std::string()));
			json data = OnGameThread([&] { return MapCells(name); }, isError);
			return isError ? data : WriteCsv("map_" + name, data["rows"], { "x", "z", "value" });
		}
		isError = true;
		return Error("what must be buildings, lots, budget, history or map (with arguments for the matching tool, e.g. {\"graph\": \"0x06\", \"months\": 24} or {\"map\": \"land_value\"}).");
	}

	// ------------------------------------------------------------------------------------------------- region

	std::string Text(cIGZString* text) { return text ? text->ToChar() : std::string(); }

	json GetRegion()
	{
		cISC4AppPtr app;
		cISC4Region* region = app ? app->GetRegion() : nullptr;
		if (!region) return Error("No region is loaded.");
		json cities = json::array();
		long long population = 0, commercial = 0, industrial = 0;
		{
			SC4List<cRZAutoRefCount<cISC4RegionalCity>> list;
			region->GetAllCities(list);
			for (const auto& entry : list)
			{
				cISC4RegionalCity* city = entry;
				if (!city) continue;
				int32_t x = 0, z = 0, sx = 0, sz = 0;
				city->GetPosition(x, z);
				city->GetCitySize(sx, sz);
				cRZBaseString name;
				city->GetCityName(name);
				const bool established = city->GetEstablished();
				json c{ {"name", name.ToChar()}, {"position", json{ {"x", x}, {"z", z} }}, {"size", json{ {"x", sx}, {"z", sz} }}, {"established", established} };
				if (established)
				{
					c["population"] = city->GetPopulation();
					c["commercial_jobs"] = city->GetCommercialJobs();
					c["industrial_jobs"] = city->GetIndustrialJobs();
					c["mayor_rating"] = city->GetMayorRating();
					population += city->GetPopulation();
					commercial += city->GetCommercialJobs();
					industrial += city->GetIndustrialJobs();
				}
				cities.push_back(c);
			}
		}
		return json{ {"region", Text(region->GetName())}, {"cities", cities},
			{"totals", json{ {"population", population}, {"commercial_jobs", commercial}, {"industrial_jobs", industrial} }},
			{"note", "Figures of the other cities are those saved in the region the last time each city was played."} };
	}

	// ------------------------------------------------------------------------------------------------- audit

	json AuditBuildings(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		cISC4PowerSimulator* power = city->GetPowerSimulator();
		const std::string only = Lower(arguments.value("family", std::string()));
		struct Row { std::string family, name, exemplar; int count = 0; double capacity = 0, monthly = 0, plop = 0, radius = 0; };
		std::map<std::string, Row> rows; // by exemplar
		for (cISC4Occupant* occupant : AllBuildings(city))
		{
			std::string family;
			double capacity = 0, radius = 0;
			uint32_t value = 0;
			if (Prop(occupant, kPropStudents, value) && value) { family = "school"; capacity = value; if (Prop(occupant, kPropSchoolRadius, value)) radius = value; }
			else if (Prop(occupant, kPropPatients, value) && value) { family = "hospital"; capacity = value; if (Prop(occupant, kPropHospitalRadius, value)) radius = value; }
			else if (Prop(occupant, kPropFireRadius, value) && value) { family = "fire"; radius = value; }
			else if (Prop(occupant, kPropPoliceRadius, value) && value) { family = "police"; radius = value; }
			else if (power)
			{
				uint32_t maximum = 0;
				power->GetOccupantMaxPowerProduced(occupant, maximum);
				if (maximum > 0) { family = "power"; capacity = maximum; }
			}
			if (family.empty() || (!only.empty() && only != family)) continue;
			const std::string exemplar = Hex(BuildingType(occupant));
			Row& row = rows[exemplar];
			if (row.count == 0)
			{
				row.family = family;
				row.name = BuildingName(occupant);
				row.exemplar = exemplar;
				row.capacity = capacity;
				row.radius = radius;
				row.monthly = static_cast<double>(CostOf(budget, occupant).monthly);
				if (Prop(occupant, kPropPlopCost, value)) row.plop = value;
			}
			++row.count;
		}
		std::map<std::string, std::vector<const Row*>> byFamily;
		for (const auto& [exemplar, row] : rows) byFamily[row.family].push_back(&row);
		json families = json::object();
		json flagged = json::array();
		for (const auto& [family, list] : byFamily)
		{
			std::vector<double> perUnit;
			for (const Row* row : list) if (row->capacity > 0 && row->monthly > 0) perUnit.push_back(row->monthly / row->capacity);
			std::sort(perUnit.begin(), perUnit.end());
			const double median = perUnit.empty() ? 0.0 : perUnit[perUnit.size() / 2];
			json buildings = json::array();
			for (const Row* row : list)
			{
				json b{ {"name", row->name}, {"exemplar", row->exemplar}, {"in_city", row->count}, {"monthly_cost", row->monthly} };
				if (row->capacity > 0) b["capacity"] = row->capacity;
				if (row->radius > 0) b["radius_m"] = row->radius;
				if (row->plop > 0) b["plop_cost"] = row->plop;
				json reasons = json::array();
				if (row->capacity > 0 && row->monthly > 0)
				{
					const double unit = row->monthly / row->capacity;
					b["cost_per_unit"] = unit;
					if (median > 0 && unit > 2.0 * median) reasons.push_back("costs more than twice the family median per unit");
					if (median > 0 && unit < 0.5 * median) reasons.push_back("costs less than half the family median per unit");
				}
				if (row->plop > 0 && row->monthly > 0)
				{
					const double months = row->plop / row->monthly;
					b["plop_in_months_of_upkeep"] = std::round(months * 10.0) / 10.0;
					if (months < 3) reasons.push_back("built for less than 3 months of upkeep (BSC-style plop cost)");
					if (months > 120) reasons.push_back("plop cost above 10 years of upkeep");
				}
				if (row->monthly <= 0) reasons.push_back("no monthly cost in the budget");
				if (!reasons.empty())
				{
					b["flags"] = reasons;
					flagged.push_back(json{ {"family", family}, {"name", row->name}, {"exemplar", row->exemplar}, {"flags", reasons} });
				}
				buildings.push_back(b);
			}
			families[family] = json{ {"median_cost_per_unit", median}, {"buildings", buildings} };
		}
		return json{ {"families", families}, {"flagged", flagged},
			{"note", "One row per exemplar in the city. Capacity: students, patients or MW. Flags compare each building with the median of its family in this city and the plop cost with the monthly cost; get_building_properties shows all of a building's values."} };
	}

	// ------------------------------------------------------------------------------------------------- resources

	// Runs on the HTTP thread: the resource manager is read on the game thread, properties through get_building_properties.
	json ReadResource(const json& arguments, bool& isError)
	{
		const uint32_t type = ParseId(arguments.at("type")), group = ParseId(arguments.at("group")), instance = ParseId(arguments.at("instance"));
		const json key{ {"type", Hex(type)}, {"group", Hex(group)}, {"instance", Hex(instance)} };
		json found = OnGameThread([&]() -> json
		{
			cIGZPersistResourceManagerPtr resources;
			if (!resources) return Error("Resource manager unavailable.");
			const cGZPersistResourceKey resourceKey(type, group, instance);
			json result{ {"exists", resources->TestForKey(resourceKey)} };
			if (!result["exists"].get<bool>()) return result;
			if (type == 0x2026960B) { result["text"] = LocalizedText(group, instance); return result; }
			if (type != 0x6534284A && type != 0x05342861) return result;
			// The parent cohorts, nearest first.
			json chain = json::array();
			cRZAutoRefCount<cISCResExemplar> exemplar;
			if (resources->GetResource(resourceKey, GZIID_cISCResExemplar, exemplar.AsPPVoid(), 0, nullptr) && exemplar)
			{
				cISCResExemplarCohort* cohort = exemplar->GetCohort();
				for (int depth = 0; cohort && depth < 8; ++depth, cohort = cohort->GetParent())
				{
					cGZPersistResourceKey cohortKey;
					if (cohort->GetKey(cohortKey)) chain.push_back(json{ {"type", Hex(cohortKey.type)}, {"group", Hex(cohortKey.group)}, {"instance", Hex(cohortKey.instance)} });
				}
			}
			result["chain"] = chain;
			return result;
		}, isError);
		if (isError || found.contains("error")) return found;
		json out{ {"key", key}, {"exists", found["exists"]} };
		if (!found["exists"].get<bool>()) return out;
		if (found.contains("text")) { out["kind"] = "LTEXT"; out["text"] = found["text"]; return out; }
		if (!found.contains("chain"))
		{
			out["kind"] = "other";
			out["note"] = "This resource exists; only exemplars, cohorts and LTEXT are decoded here.";
			return out;
		}
		out["kind"] = type == 0x6534284A ? "exemplar" : "cohort";
		bool callError = false;
		json local = CallTool("get_building_properties", key, callError);
		out["properties"] = local.value("properties", json::array());
		std::set<std::string> present;
		for (const json& property : out["properties"]) present.insert(property.value("id", std::string()));
		json inherited = json::array();
		for (const json& cohort : found["chain"])
		{
			json cohortProps = CallTool("get_building_properties", cohort, callError);
			for (json property : cohortProps.value("properties", json::array()))
			{
				if (!present.insert(property.value("id", std::string())).second) continue;
				property["from_cohort"] = cohort["instance"];
				inherited.push_back(property);
			}
		}
		out["parent_cohorts"] = found["chain"];
		out["inherited_properties"] = inherited;
		return out;
	}

	// ------------------------------------------------------------------------------------------------- plugins

	std::string FileVersion(const fs::path& path)
	{
		DWORD handle = 0;
		const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
		if (size == 0) return {};
		std::vector<uint8_t> data(size);
		if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
		VS_FIXEDFILEINFO* info = nullptr;
		UINT length = 0;
		if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return {};
		char text[64];
		std::snprintf(text, sizeof(text), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
		return text;
	}

	bool IsPluginFile(const fs::path& path)
	{
		const std::string extension = Lower(path.extension().string());
		return extension == ".dat" || extension == ".sc4lot" || extension == ".sc4desc" || extension == ".sc4model";
	}

	json ListPlugins(const json& arguments)
	{
		const fs::path root = McpLog::DllFolder();
		std::map<std::string, std::pair<int, uintmax_t>> byExtension;
		std::map<std::string, int> byFolder;
		int files = 0;
		std::error_code error;
		for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error); it != fs::recursive_directory_iterator(); it.increment(error))
		{
			if (error) { error.clear(); continue; }
			if (!it->is_regular_file(error)) continue;
			++files;
			const std::string extension = Lower(it->path().extension().string());
			auto& entry = byExtension[extension.empty() ? "(none)" : extension];
			entry.first++;
			entry.second += it->file_size(error);
			const fs::path relative = fs::relative(it->path(), root, error);
			if (IsPluginFile(it->path())) byFolder[relative.has_parent_path() ? relative.begin()->string() : std::string("(Plugins root)")]++;
		}
		json dlls = json::array();
		for (const auto& entry : fs::directory_iterator(root, error))
		{
			if (Lower(entry.path().extension().string()) != ".dll") continue;
			fs::path log = entry.path(), ini = entry.path();
			log.replace_extension(".log");
			ini.replace_extension(".ini");
			dlls.push_back(json{ {"file", entry.path().filename().string()}, {"version", FileVersion(entry.path())}, {"size", entry.file_size(error)},
				{"log", fs::exists(log) ? log.filename().string() : std::string()}, {"ini", fs::exists(ini) ? ini.filename().string() : std::string()} });
		}
		json extensions = json::object();
		for (const auto& [extension, entry] : byExtension) extensions[extension] = json{ {"files", entry.first}, {"bytes", entry.second} };
		json folders = json::array();
		for (const auto& [folder, count] : byFolder) folders.push_back(json{ {"folder", folder}, {"plugin_files", count} });
		const size_t limit = static_cast<size_t>(std::clamp(arguments.value("limit", 200), 1, 5000));
		if (folders.size() > limit) folders.erase(folders.begin() + static_cast<long>(limit), folders.end());
		return json{ {"plugins_folder", root.string()}, {"files", files}, {"by_extension", extensions}, {"dlls", dlls}, {"top_folders", folders},
			{"note", "DLLs load only from the top level of Plugins. The game's own Plugins folder (in the installation folder) is not listed."} };
	}

	// DBPF index scan, to find which plugin files define the same resources (later files override earlier ones).
	struct TgiKey
	{
		uint32_t type, group, instance;
		bool operator==(const TgiKey& o) const { return type == o.type && group == o.group && instance == o.instance; }
	};
	struct TgiHash { size_t operator()(const TgiKey& k) const { return (static_cast<size_t>(k.type) * 0x9E3779B1u) ^ (static_cast<size_t>(k.group) << 7) ^ k.instance; } };

	struct PluginIndex
	{
		std::vector<std::string> files;                                  // in estimated load order
		std::unordered_map<TgiKey, std::vector<uint32_t>, TgiHash> where; // file indexes, in load order
		std::chrono::steady_clock::time_point built;
		size_t entries = 0;
		int unreadable = 0;
	};
	std::mutex indexMutex;
	PluginIndex pluginIndex;

	bool ReadDbpfIndex(const fs::path& path, std::vector<TgiKey>& out)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file) return false;
		uint8_t header[96]{};
		if (!file.read(reinterpret_cast<char*>(header), 96) || std::memcmp(header, "DBPF", 4) != 0) return false;
		auto u32 = [&](size_t at) { return static_cast<uint32_t>(header[at]) | (header[at + 1] << 8) | (header[at + 2] << 16) | (static_cast<uint32_t>(header[at + 3]) << 24); };
		const uint32_t count = u32(0x24), offset = u32(0x28), size = u32(0x2C), indexMinor = u32(0x3C);
		if (count == 0 || count > 2000000) return true;
		const uint32_t entrySize = indexMinor >= 1 ? 24 : 20;
		if (size < count * entrySize && size != 0) return false;
		std::vector<uint8_t> index(static_cast<size_t>(count) * entrySize);
		file.seekg(offset);
		if (!file.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size()))) return false;
		for (uint32_t i = 0; i < count; ++i)
		{
			const uint8_t* e = index.data() + static_cast<size_t>(i) * entrySize;
			auto v = [&](int k) { return static_cast<uint32_t>(e[k]) | (e[k + 1] << 8) | (e[k + 2] << 16) | (static_cast<uint32_t>(e[k + 3]) << 24); };
			const TgiKey key{ v(0), v(4), v(8) };
			if (key.type == 0xE86B1EEF) continue; // directory of compressed entries
			out.push_back(key);
		}
		return true;
	}

	// SC4 loads the files of a folder before its subfolders, each level in alphabetical order; a later file wins.
	bool LoadOrderLess(const fs::path& a, const fs::path& b)
	{
		auto ia = a.begin(), ib = b.begin();
		for (; ia != a.end() && ib != b.end(); ++ia, ++ib)
		{
			const bool lastA = std::next(ia) == a.end(), lastB = std::next(ib) == b.end();
			if (*ia == *ib) continue;
			if (lastA != lastB) return lastA; // a file comes before a subfolder at the same level
			return Lower(ia->string()) < Lower(ib->string());
		}
		return a.native().size() < b.native().size();
	}

	const PluginIndex& BuildIndex(bool refresh)
	{
		if (!refresh && !pluginIndex.files.empty() && std::chrono::steady_clock::now() - pluginIndex.built < std::chrono::minutes(10)) return pluginIndex;
		PluginIndex index;
		const fs::path root = McpLog::DllFolder();
		std::vector<fs::path> paths;
		std::error_code error;
		for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, error); it != fs::recursive_directory_iterator(); it.increment(error))
		{
			if (error) { error.clear(); continue; }
			if (it->is_regular_file(error) && IsPluginFile(it->path())) paths.push_back(fs::relative(it->path(), root, error));
		}
		std::sort(paths.begin(), paths.end(), LoadOrderLess);
		for (const fs::path& relative : paths)
		{
			std::vector<TgiKey> keys;
			if (!ReadDbpfIndex(root / relative, keys)) { ++index.unreadable; continue; }
			const uint32_t fileIndex = static_cast<uint32_t>(index.files.size());
			index.files.push_back(relative.string());
			for (const TgiKey& key : keys)
			{
				auto& list = index.where[key];
				if (list.empty() || list.back() != fileIndex) list.push_back(fileIndex);
			}
			index.entries += keys.size();
		}
		index.built = std::chrono::steady_clock::now();
		pluginIndex = std::move(index);
		McpLog::Write("Plugin index: %u files, %u entries.", static_cast<unsigned>(pluginIndex.files.size()), static_cast<unsigned>(pluginIndex.entries));
		return pluginIndex;
	}

	json FindPluginConflicts(const json& arguments)
	{
		std::lock_guard<std::mutex> lock(indexMutex);
		const PluginIndex& index = BuildIndex(arguments.value("refresh", false));
		const bool filterType = arguments.contains("type"), filterGroup = arguments.contains("group");
		const uint32_t type = filterType ? ParseId(arguments["type"]) : 0, group = filterGroup ? ParseId(arguments["group"]) : 0;
		const std::string fileFilter = Lower(arguments.value("file_contains", std::string()));
		const size_t limit = static_cast<size_t>(std::clamp(arguments.value("limit", 50), 1, 1000));

		// Group overridden resources by (winner, loser) file pair.
		std::map<std::pair<uint32_t, uint32_t>, std::pair<size_t, TgiKey>> pairs;
		size_t overridden = 0;
		for (const auto& [key, files] : index.where)
		{
			if (files.size() < 2) continue;
			if (filterType && key.type != type) continue;
			if (filterGroup && key.group != group) continue;
			const uint32_t winner = files.back();
			for (size_t i = 0; i + 1 < files.size(); ++i)
			{
				if (!fileFilter.empty() && Lower(index.files[winner]).find(fileFilter) == std::string::npos && Lower(index.files[files[i]]).find(fileFilter) == std::string::npos) continue;
				auto& entry = pairs[{ winner, files[i] }];
				if (entry.first++ == 0) entry.second = key;
				++overridden;
			}
		}
		std::vector<std::pair<std::pair<uint32_t, uint32_t>, std::pair<size_t, TgiKey>>> sorted(pairs.begin(), pairs.end());
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
		json list = json::array();
		for (size_t i = 0; i < sorted.size() && i < limit; ++i)
		{
			const TgiKey& example = sorted[i].second.second;
			list.push_back(json{ {"wins", index.files[sorted[i].first.first]}, {"overrides", index.files[sorted[i].first.second]},
				{"resources", sorted[i].second.first},
				{"example", json{ {"type", Hex(example.type)}, {"group", Hex(example.group)}, {"instance", Hex(example.instance)} }} });
		}
		return json{ {"plugin_files", index.files.size()}, {"unreadable_files", index.unreadable}, {"entries", index.entries},
			{"overridden_resources", overridden}, {"file_pairs", pairs.size()}, {"returned", list.size()}, {"overrides", list},
			{"note", "Load order is estimated (files of a folder before its subfolders, alphabetical, the later file wins). Overrides are often intended (patches, mods loaded last on purpose); look for the unexpected ones. Filter with type, group or file_contains."} };
	}

	json FindResourceFile(const json& arguments)
	{
		std::lock_guard<std::mutex> lock(indexMutex);
		const PluginIndex& index = BuildIndex(arguments.value("refresh", false));
		const TgiKey key{ ParseId(arguments.at("type")), ParseId(arguments.at("group")), ParseId(arguments.at("instance")) };
		const auto found = index.where.find(key);
		json files = json::array();
		if (found != index.where.end()) for (uint32_t file : found->second) files.push_back(index.files[file]);
		json out{ {"key", json{ {"type", Hex(key.type)}, {"group", Hex(key.group)}, {"instance", Hex(key.instance)} }}, {"files_in_load_order", files} };
		out["winner"] = files.empty() ? json(nullptr) : files.back();
		if (files.empty()) out["note"] = "Not in any plugin file: it comes from the game's own files (SimCity_1.dat...) or does not exist.";
		return out;
	}

	// ------------------------------------------------------------------------------------------------- cheats

	json ListCheats()
	{
		cISC4AppPtr app;
		cIGZCheatCodeManager* cheats = app ? app->GetCheatCodeManager() : nullptr;
		if (!cheats) return Error("Cheat code manager unavailable.");
		cIGZString* strings[1024]{};
		uint32_t count = 1024;
		cheats->GetListOfAllCheatStrings(strings, count);
		count = std::min<uint32_t>(count, 1024);
		std::set<std::string> names;
		for (uint32_t i = 0; i < count; ++i) if (strings[i]) names.insert(strings[i]->ToChar());
		json list = json::array();
		for (const std::string& name : names)
		{
			cRZBaseString line(name.c_str());
			uint32_t id = 0;
			const bool known = cheats->DoesCheatCodeMatch(line, id);
			list.push_back(json{ {"cheat", name}, {"id", known ? Hex(id) : std::string()} });
		}
		return json{ {"cheats", list}, {"count", list.size()}, {"note", "Cheats of the game and of the DLL plugins, as registered now; run_cheat runs one."} };
	}

	// ------------------------------------------------------------------------------------------------- scenarios

	// Follows a path like "status.funds" or "departments.0.funding_percent" into a result.
	const json* Follow(const json& value, const std::string& path)
	{
		const json* current = &value;
		size_t start = 0;
		while (start <= path.size() && !path.empty())
		{
			const size_t dot = path.find('.', start);
			const std::string part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
			if (current->is_object())
			{
				if (!current->contains(part)) return nullptr;
				current = &(*current)[part];
			}
			else if (current->is_array())
			{
				size_t index = 0;
				try { index = std::stoul(part); } catch (...) { return nullptr; }
				if (index >= current->size()) return nullptr;
				current = &(*current)[index];
			}
			else return nullptr;
			if (dot == std::string::npos) break;
			start = dot + 1;
		}
		return current;
	}

	bool Compare(const json& actual, const std::string& op, const json& expected)
	{
		if (op == "exists") return true;
		if (actual.is_number() && expected.is_number())
		{
			const double a = actual.get<double>(), b = expected.get<double>();
			if (op == ">") return a > b;
			if (op == ">=") return a >= b;
			if (op == "<") return a < b;
			if (op == "<=") return a <= b;
			if (op == "==") return a == b;
			if (op == "!=") return a != b;
		}
		if (op == "==") return actual == expected;
		if (op == "!=") return actual != expected;
		if (op == "contains" && actual.is_string() && expected.is_string()) return actual.get<std::string>().find(expected.get<std::string>()) != std::string::npos;
		return false;
	}

	std::string Brief(const json& value)
	{
		std::string text = value.dump();
		return text.size() > 400 ? text.substr(0, 400) + "..." : text;
	}

	json RunScenario(const json& arguments, bool& isError, const Tools::Context& context)
	{
		const json& steps = arguments.at("steps");
		if (!steps.is_array() || steps.empty() || steps.size() > 50) { isError = true; return Error("steps must be a list of 1 to 50 steps."); }
		const bool stopOnFailure = arguments.value("stop_on_failure", true);

		// One confirmation for the whole scenario when it changes the city.
		static const std::set<std::string> kChanging = { "set_funding", "set_building_funding", "set_tax", "set_ordinance", "take_loan", "undo_last_change",
			"place_zone", "run_cheat", "save_city" };
		std::vector<std::string> changing;
		for (const json& step : steps)
		{
			const std::string tool = step.value("tool", std::string());
			if (kChanging.count(tool)) changing.push_back(tool);
		}
		if (arguments.value("dry_run", false))
		{
			return json{ {"dry_run", true}, {"steps", steps.size()}, {"changes_the_city", changing} };
		}
		if (!changing.empty() && ConfirmChangesEnabled() && !arguments.value("confirm", false))
		{
			std::string list;
			for (const std::string& tool : changing) list += (list.empty() ? "" : ", ") + tool;
			const int answer = context.confirm ? context.confirm("Run a scenario of " + std::to_string(steps.size()) + " steps that changes the city (" + list + ")?") : -1;
			if (answer == 0) return json{ {"cancelled", true} };
			if (answer < 0) return json{ {"confirmation_required", true}, {"changes_the_city", changing}, {"note", "Nothing was run. Call again with confirm=true to run the scenario."} };
		}

		json results = json::array();
		int passed = 0, failed = 0;
		json last = json::object();
		for (size_t i = 0; i < steps.size(); ++i)
		{
			const json& step = steps[i];
			json record{ {"step", i + 1} };
			bool stepFailed = false;
			if (step.contains("wait_months"))
			{
				bool callError = false;
				json args{ {"months", step["wait_months"]}, {"timeout_seconds", step.value("timeout_seconds", 300)} };
				if (step.contains("speed")) args["speed"] = step["speed"];
				last = CallTool("wait_months", args, callError);
				record["action"] = "wait_months";
				record["result"] = Brief(last);
				stepFailed = callError;
			}
			else if (step.contains("tool"))
			{
				bool callError = false;
				json args = step.value("arguments", json::object());
				if (kChanging.count(step["tool"].get<std::string>())) args["confirm"] = true;
				last = CallTool(step["tool"].get<std::string>(), args, callError);
				record["action"] = step["tool"];
				record["result"] = Brief(last);
				stepFailed = callError;
			}
			if (step.contains("check"))
			{
				const json& check = step["check"];
				json subject = last;
				if (check.contains("tool"))
				{
					bool callError = false;
					subject = CallTool(check["tool"].get<std::string>(), check.value("arguments", json::object()), callError);
				}
				const std::string path = check.value("path", std::string());
				const std::string op = check.value("op", std::string("exists"));
				const json* actual = Follow(subject, path);
				const bool ok = actual && Compare(*actual, op, check.value("value", json()));
				record["check"] = json{ {"path", path}, {"op", op}, {"expected", check.value("value", json())}, {"actual", actual ? *actual : json(nullptr)}, {"passed", ok} };
				if (!ok) stepFailed = true;
			}
			record["passed"] = !stepFailed;
			++(stepFailed ? failed : passed);
			results.push_back(record);
			if (context.progress) context.progress(static_cast<double>(i + 1), static_cast<double>(steps.size()), "Step " + std::to_string(i + 1) + (stepFailed ? " failed" : " passed"));
			if (stepFailed && stopOnFailure) break;
		}
		McpLog::Write("run_scenario: %d passed, %d failed", passed, failed);
		return json{ {"passed", passed}, {"failed", failed}, {"steps_run", results.size()}, {"steps_total", steps.size()}, {"result", failed == 0 ? "PASS" : "FAIL"}, {"steps", results} };
	}
}

namespace ToolsCommon
{
	void AddMoreTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "suggest_location",
			"Where to build the next school, hospital, fire or police station: the areas with the most residents (or residents and workers) outside the coverage of the existing buildings of that kind, with a free cell nearby. radius_m defaults to the median radius of the existing buildings.",
			Schema({ {"service", {{"type", "string"}, {"description", "school, hospital, fire or police."}}}, {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}},
				{"radius_m", {{"type", "number"}, {"minimum", 32}}} }, json::array({ "service" })), Permission::Read,
			GameThread([](const json& arguments) { return SuggestLocation(arguments); }) });

		tools.push_back({ "export_csv",
			"Writes data to a CSV file (UTF-8, for Excel or other tools) in SC4MCP_exports next to the DLL: what = buildings, lots, budget, history or map; arguments are passed to the matching tool (e.g. {\"graph\": \"0x06\", \"months\": 24} for history, {\"map\": \"land_value\"} for map, filters for buildings and lots).",
			Schema({ {"what", {{"type", "string"}}}, {"arguments", {{"type", "object"}}} }, json::array({ "what" })), Permission::Read,
			[](const json& arguments, bool& isError, const Tools::Context&) { return ExportCsv(arguments, isError); } });

		tools.push_back({ "get_region",
			"The region: every city with position, size and, for established cities, population, commercial and industrial jobs and mayor rating, plus the region totals.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetRegion(); }) });

		tools.push_back({ "audit_buildings",
			"Checks the civic and utility buildings of the city family by family (school, hospital, fire, police, power): capacity, monthly cost, plop cost, radius, cost per unit, and flags buildings far from their family median or with BSC-style plop costs. family limits it to one family.",
			Schema({ {"family", {{"type", "string"}}} }), Permission::Read, GameThread([](const json& arguments) { return AuditBuildings(arguments); }) });

		tools.push_back({ "read_resource",
			"Reads a game resource by key, as the game loaded it: exemplars and cohorts with their properties and the properties inherited from parent cohorts, LTEXT as text; for other types it says whether the resource exists.",
			Schema({ {"type", {{"type", "string"}}}, {"group", {{"type", "string"}}}, {"instance", {{"type", "string"}}} }, json::array({ "type", "group", "instance" })), Permission::Read,
			[](const json& arguments, bool& isError, const Tools::Context&) { return ReadResource(arguments, isError); } });

		tools.push_back({ "list_plugins",
			"The Plugins folder: number of files and bytes by extension, the plugin files of each top-level folder, and every DLL with its version, log and ini.",
			Schema({ {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}}} }), Permission::Read,
			[](const json& arguments, bool&, const Tools::Context&) { return ListPlugins(arguments); } });

		tools.push_back({ "find_plugin_conflicts",
			"Reads the index of every plugin file and lists the resources defined by more than one file, grouped by file pair (which file wins in the estimated load order, which one it overrides, how many resources, an example). Filters: type, group, file_contains. The index is kept for 10 minutes; refresh=true rescans.",
			Schema({ {"type", {{"type", "string"}}}, {"group", {{"type", "string"}}}, {"file_contains", {{"type", "string"}}},
				{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}, {"refresh", {{"type", "boolean"}}} }), Permission::Read,
			[](const json& arguments, bool&, const Tools::Context&) { return FindPluginConflicts(arguments); } });

		tools.push_back({ "find_resource_file",
			"Which plugin files define a resource (type, group, instance), in the estimated load order, and which one the game uses.",
			Schema({ {"type", {{"type", "string"}}}, {"group", {{"type", "string"}}}, {"instance", {{"type", "string"}}}, {"refresh", {{"type", "boolean"}}} },
				json::array({ "type", "group", "instance" })), Permission::Read,
			[](const json& arguments, bool&, const Tools::Context&) { return FindResourceFile(arguments); } });

		tools.push_back({ "list_cheats",
			"Every cheat code registered now, by the game and by the DLL plugins.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return ListCheats(); }) });

		tools.push_back({ "run_scenario",
			"Runs a test scenario: a list of steps, each {\"tool\": name, \"arguments\": {...}}, {\"wait_months\": n} and/or {\"check\": {\"tool\"?: name, \"arguments\"?: {...}, \"path\": \"status.funds\", \"op\": \">\", \"value\": 0}} (ops: > >= < <= == != contains exists; without a tool the check reads the result of the step). Returns PASS or FAIL with every step. One confirmation for the whole scenario when it changes the city; dry_run=true lists what it would change.",
			Schema({ {"steps", {{"type", "array"}, {"items", {{"type", "object"}}}}}, {"stop_on_failure", {{"type", "boolean"}}},
				{"dry_run", {{"type", "boolean"}}}, {"confirm", {{"type", "boolean"}}} }, json::array({ "steps" })), Permission::Control,
			[](const json& arguments, bool& isError, const Tools::Context& context) { return RunScenario(arguments, isError, context); } });
	}
}
