// SC4MCP - helpers shared by the tool modules.
#include "ToolsCommon.h"

#include <windows.h>
#include "MainThreadQueue.h"

#include "cIGZString.h"
#include "cISC4AdvisorSystem.h"
#include "cISC4App.h"
#include "cISC4BudgetSimulator.h"
#include "cISC4BuildingOccupant.h"
#include "cISC4City.h"
#include "cISC4DepartmentBudget.h"
#include "cISC4Lot.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISC4OccupantFilter.h"
#include "cISC4OccupantManager.h"
#include "cISC4Simulator.h"
#include "cISC4CrimeSimulator.h"
#include "cISC4FireProtectionSimulator.h"
#include "cISC4LandValueSimulator.h"
#include "cISC4PoliceSimulator.h"
#include "cISC4PollutionSimulator.h"
#include "cISC4PowerSimulator.h"
#include "cISC4SimGrid.h"
#include "cISC4TrafficSimulator.h"
#include "McpLog.h"
#include "cISCLua.h"
#include "cISCPropertyHolder.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"
#include "SC4Rect.h"
#include "SC4Vector.h"
#include "StringResourceKey.h"
#include "StringResourceManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

using nlohmann::json;

namespace
{
	constexpr uint32_t kOccupantTypeBuilding = 0x278128A0;

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

	struct ZoneInfo { int32_t id; const char* name; std::vector<const char*> aliases; };
	const std::vector<ZoneInfo>& Zones()
	{
		static const std::vector<ZoneInfo> zones = {
			{ 0, "none", { "dezone", "empty" } },
			{ 1, "residential_low", { "r_low", "rl", "r1" } },
			{ 2, "residential_medium", { "r_medium", "r_med", "rm", "r2" } },
			{ 3, "residential_high", { "r_high", "rh", "r3" } },
			{ 4, "commercial_low", { "c_low", "cl", "c1" } },
			{ 5, "commercial_medium", { "c_medium", "c_med", "cm", "c2" } },
			{ 6, "commercial_high", { "c_high", "ch", "c3" } },
			{ 7, "agriculture", { "farm", "i_agriculture", "ia" } },
			{ 8, "industrial_medium", { "i_medium", "i_med", "im", "i2" } },
			{ 9, "industrial_high", { "i_high", "ih", "i3" } },
			{ 10, "military", {} },
			{ 11, "airport", {} },
			{ 12, "seaport", {} },
			{ 13, "spaceport", {} },
			{ 14, "landfill", {} },
			{ 15, "plopped", {} },
		};
		return zones;
	}
}

namespace ToolsCommon
{
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

	json Schema(json properties, json required)
	{
		if (properties.is_null()) properties = json::object();
		return json{ {"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)} };
	}

	json Error(const std::string& message)
	{
		return json{ {"error", message} };
	}

	uint32_t ParseId(const json& value)
	{
		if (value.is_number_unsigned()) return value.get<uint32_t>();
		if (value.is_number_integer()) return static_cast<uint32_t>(value.get<int64_t>());
		if (value.is_string())
		{
			const std::string text = value.get<std::string>();
			size_t used = 0;
			const unsigned long result = std::stoul(text, &used, 0);
			if (used != text.size()) throw std::invalid_argument("not a number: " + text);
			return static_cast<uint32_t>(result);
		}
		throw std::invalid_argument("expected a number or a hex string");
	}

	json OnGameThread(const std::function<json()>& work, bool& isError)
	{
		std::string text;
		const bool ran = MainThreadQueue::Run([&]() -> std::string
		{
			try { return work().dump(); }
			catch (const std::exception& error) { return Error(std::string("Game thread: ") + error.what()).dump(); }
		}, text);
		if (!ran)
		{
			isError = true;
			return Error("The game did not answer in time (loading, minimized or a dialog is open).");
		}
		json result = json::parse(text, nullptr, false);
		if (result.is_discarded())
		{
			isError = true;
			return Error("Invalid result from the game thread.");
		}
		if (result.is_object() && result.contains("error")) isError = true;
		return result;
	}

	Handler GameThread(std::function<json(const json& arguments)> work)
	{
		return [work](const json& arguments, bool& isError, const Tools::Context&) -> json
		{
			return OnGameThread([&] { return work(arguments); }, isError);
		};
	}

	cISC4City* City()
	{
		cISC4AppPtr app;
		return app ? app->GetCity() : nullptr;
	}

	json NoCity()
	{
		return json{ {"city_loaded", false}, {"error", "No city is open: open a city in SimCity 4 first."} };
	}

	json DateOf(cISC4Simulator* simulator)
	{
		uint32_t year = 0, month = 0, day = 0, dayOfYear = 0, weekDay = 0;
		simulator->GetSimDate(&year, &month, &day, &dayOfYear, &weekDay);
		char text[32];
		std::snprintf(text, sizeof(text), "%04u-%02u-%02u", year, month, day);
		return json{ {"year", year}, {"month", month}, {"day", day}, {"text", text}, {"day_number", simulator->GetSimDateNumber()} };
	}

	bool InCity(cISC4City* city, int32_t x, int32_t z)
	{
		return city && x >= 0 && z >= 0 && x < static_cast<int32_t>(city->CellCountX()) && z < static_cast<int32_t>(city->CellCountZ());
	}

	std::vector<cISC4Occupant*> AllBuildings(cISC4City* city)
	{
		Collected collected;
		cISC4OccupantManager* occupants = city ? city->GetOccupantManager() : nullptr;
		if (!occupants) return {};
		BuildingFilter filter;
		occupants->IterateOccupants(Collect, &collected, nullptr, nullptr, &filter);
		return collected.buildings;
	}

	std::string BuildingName(cISC4Occupant* occupant)
	{
		cRZAutoRefCount<cISC4BuildingOccupant> building;
		if (!occupant || !occupant->QueryInterface(GZIID_cISC4BuildingOccupant, building.AsPPVoid())) return {};
		cRZBaseString name;
		if (building->GetName(name) && name.Strlen() > 0) return name.ToChar();
		if (cIGZString* exemplarName = building->GetExemplarName()) return exemplarName->ToChar();
		return {};
	}

	uint32_t BuildingType(cISC4Occupant* occupant)
	{
		cRZAutoRefCount<cISC4BuildingOccupant> building;
		if (!occupant || !occupant->QueryInterface(GZIID_cISC4BuildingOccupant, building.AsPPVoid())) return 0;
		return building->GetBuildingType();
	}

	json BuildingCells(cISC4Occupant* occupant)
	{
		SC4Rect<long> cells{};
		if (!occupant || !occupant->GetBoundingCityCells(cells)) return nullptr;
		return json{ {"x1", cells.topLeftX}, {"z1", cells.topLeftY}, {"x2", cells.bottomRightX}, {"z2", cells.bottomRightY} };
	}

	BuildingCost CostOf(cISC4BudgetSimulator* budget, cISC4Occupant* occupant)
	{
		BuildingCost cost;
		cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
		if (!budget || !holder) return cost;
		SC4Vector<cISC4BudgetSimulator::BudgetItem> items;
		if (!budget->GetBudgetItemInfo(holder, items)) return cost;
		for (const auto& item : items)
		{
			cost.items.push_back({ item.departmentID, item.lineItem, item.purpose, item.cost, item.localFundingPercentage });
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

	cISC4Occupant* BuildingAt(cISC4City* city, int32_t x, int32_t z, cISC4Lot** lotOut)
	{
		if (lotOut) *lotOut = nullptr;
		cISC4LotManager* lots = city ? city->GetLotManager() : nullptr;
		if (!lots || !InCity(city, x, z)) return nullptr;
		cISC4Lot* lot = lots->GetLot(x, z, false);
		if (lotOut) *lotOut = lot;
		if (!lot) return nullptr;
		cISC4BuildingOccupant* building = lot->GetBuilding();
		if (!building) return nullptr;
		cISC4Occupant* occupant = nullptr;
		if (!building->QueryInterface(GZIID_cISC4Occupant, reinterpret_cast<void**>(&occupant)) || !occupant) return nullptr;
		occupant->Release(); // the lot keeps the building alive; we only need the pointer
		return occupant;
	}

	const char* ZoneName(int32_t zone)
	{
		for (const ZoneInfo& info : Zones())
		{
			if (info.id == zone) return info.name;
		}
		return "unknown";
	}

	int32_t ParseZone(const json& value)
	{
		if (value.is_number_integer())
		{
			const int64_t id = value.get<int64_t>();
			return id >= 0 && id <= 15 ? static_cast<int32_t>(id) : -1;
		}
		if (!value.is_string()) return -1;
		std::string text = Lower(value.get<std::string>());
		std::replace(text.begin(), text.end(), ' ', '_');
		std::replace(text.begin(), text.end(), '-', '_');
		for (const ZoneInfo& info : Zones())
		{
			if (text == info.name) return info.id;
			for (const char* alias : info.aliases)
			{
				if (text == alias) return info.id;
			}
		}
		return -1;
	}

	bool ReadGameTable(cISC4City* city, GameTable& out)
	{
		cISC4AdvisorSystem* advisors = city ? city->GetAdvisorSystem() : nullptr;
		cISCLua* lua = advisors ? advisors->GetScriptingContext() : nullptr;
		if (!lua) return false;
		const int32_t top = lua->GetTop();
		lua->GetGlobal("game");
		if (lua->Type(-1) != cIGZLua5Thread::LuaTypeTable)
		{
			lua->SetTop(top);
			return false;
		}
		lua->PushNil();
		int guard = 0;
		while (lua->Next(-2) != 0 && guard++ < 20000)
		{
			// key at -2, value at -1; only string keys (ToString on a number key would break Next).
			if (lua->Type(-2) == cIGZLua5Thread::LuaTypeString)
			{
				const char* key = lua->ToString(-2);
				const auto type = lua->Type(-1);
				if (key && type == cIGZLua5Thread::LuaTypeNumber)
				{
					const double number = lua->ToNumber(-1);
					if (std::isfinite(number)) out.numbers[key] = number;
				}
				else if (key && type == cIGZLua5Thread::LuaTypeString)
				{
					const char* text = lua->ToString(-1);
					if (text) out.strings[key] = text;
				}
				else if (key && type == cIGZLua5Thread::LuaTypeBoolean)
				{
					out.numbers[key] = lua->ToBoolean(-1) ? 1.0 : 0.0;
				}
			}
			lua->Pop(1); // keep the key for Next
		}
		lua->SetTop(top);
		return true;
	}

	json GameFields(const GameTable& table, const std::vector<std::string>& words)
	{
		auto matches = [&](const std::string& name)
		{
			if (words.empty()) return true;
			const std::string lower = Lower(name);
			for (const std::string& word : words)
			{
				if (lower.find(word) != std::string::npos) return true;
			}
			return false;
		};
		json out = json::object();
		for (const auto& [name, value] : table.numbers)
		{
			if (!matches(name)) continue;
			if (value == std::floor(value) && std::fabs(value) < 9.0e15) out[name] = static_cast<int64_t>(value);
			else out[name] = value;
		}
		for (const auto& [name, value] : table.strings)
		{
			if (matches(name)) out[name] = value;
		}
		return out;
	}

	std::string LocalizedText(uint32_t group, uint32_t instance)
	{
		if (instance == 0) return {};
		cRZAutoRefCount<cIGZString> text;
		if (StringResourceManager::GetLocalizedString(StringResourceKey(group, instance), text) && text) return text->ToChar();
		return {};
	}

	const std::vector<MapReader>& MapReaders()
	{
		static const std::vector<MapReader> maps = {
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

	const MapReader* FindMapReader(const std::string& name)
	{
		for (const MapReader& map : MapReaders()) if (name == map.name) return &map;
		return nullptr;
	}

	namespace
	{
		std::once_flag propertiesOnce;
		std::unordered_map<uint32_t, std::pair<std::string, std::string>> propertyNames;
		std::string propertiesFile;

		std::string Attribute(const std::string& tag, const char* key)
		{
			const std::string needle = std::string(key) + "=\"";
			const size_t at = tag.find(needle);
			if (at == std::string::npos) return {};
			const size_t start = at + needle.size();
			const size_t end = tag.find('"', start);
			return end == std::string::npos ? std::string() : tag.substr(start, end - start);
		}

		void LoadProperties()
		{
			std::vector<std::filesystem::path> candidates = { McpLog::DllFolder() / L"new_properties.xml" };
			wchar_t appData[MAX_PATH]{};
			if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) > 0) candidates.push_back(std::filesystem::path(appData) / L"sc4pimx" / L"new_properties.xml");
			for (const auto& path : candidates)
			{
				std::ifstream file(path, std::ios::binary);
				if (!file) continue;
				std::stringstream buffer;
				buffer << file.rdbuf();
				const std::string text = buffer.str();
				size_t at = 0;
				while ((at = text.find("<property ", at)) != std::string::npos)
				{
					const size_t end = text.find('>', at);
					if (end == std::string::npos) break;
					const std::string tag = text.substr(at, end - at);
					at = end;
					const std::string num = Attribute(tag, "num");
					if (num.empty()) continue;
					try
					{
						const uint32_t id = static_cast<uint32_t>(std::stoul(num, nullptr, 0));
						propertyNames.emplace(id, std::make_pair(Attribute(tag, "name"), Attribute(tag, "type")));
					}
					catch (...) {}
				}
				if (!propertyNames.empty())
				{
					propertiesFile = path.string();
					McpLog::Write("Property names: %u read from %s", static_cast<unsigned>(propertyNames.size()), propertiesFile.c_str());
					return;
				}
			}
		}
	}

	bool PropertyInfo(uint32_t id, std::string& name, std::string& type)
	{
		std::call_once(propertiesOnce, LoadProperties);
		const auto found = propertyNames.find(id);
		if (found == propertyNames.end()) return false;
		name = found->second.first;
		type = found->second.second;
		return true;
	}

	std::string PropertiesFileUsed()
	{
		std::call_once(propertiesOnce, LoadProperties);
		return propertiesFile;
	}

	std::string DataFolder(const char* sub)
	{
		const std::filesystem::path folder = McpLog::DllFolder() / sub;
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		return folder.string();
	}
}
