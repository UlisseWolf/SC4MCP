// SC4MCP - helpers shared by the tool modules (Tools.cpp, ToolsRead.cpp, ToolsControl.cpp).
//
// Everything that touches a game object must run on the game thread: tool handlers do their work inside
// OnGameThread (or GameThread), which posts it to MainThreadQueue and waits for the result.
#pragma once
#include "Tools.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class cISC4City;
class cISC4DepartmentBudget;
class cISC4Lot;
class cISC4Occupant;
class cISC4Simulator;
class cISC4BudgetSimulator;
class cISCLua;

namespace ToolsCommon
{
	using json = nlohmann::json;

	// What a tool may do; each level can be turned off in SC4MCP.ini.
	enum class Permission { Read, Control, Cheats, Build };

	using Handler = std::function<json(const json& arguments, bool& isError, const Tools::Progress& progress)>;

	struct ToolDef
	{
		std::string name;
		std::string description;
		json inputSchema;
		Permission permission;
		Handler run;
	};

	// Tool modules.
	void AddCoreTools(std::vector<ToolDef>& tools);
	void AddReadTools(std::vector<ToolDef>& tools);
	void AddControlTools(std::vector<ToolDef>& tools);

	// ------------------------------------------------------------------------------------------------- general
	std::string Hex(uint32_t value);
	std::string Lower(std::string text);
	json Schema(json properties, json required = json::array());
	json Error(const std::string& message);

	// Accepts 1234, "1234", "0x4D2". Throws std::invalid_argument for anything else.
	uint32_t ParseId(const json& value);

	// Runs work on the game thread and returns its JSON result; sets isError on a timeout or an "error" key.
	json OnGameThread(const std::function<json()>& work, bool& isError);

	// A handler that runs work on the game thread.
	Handler GameThread(std::function<json(const json& arguments)> work);

	// ------------------------------------------------------------------------------------------------- city
	cISC4City* City();
	json NoCity();
	json DateOf(cISC4Simulator* simulator);
	bool InCity(cISC4City* city, int32_t x, int32_t z);

	std::vector<cISC4Occupant*> AllBuildings(cISC4City* city);
	std::string BuildingName(cISC4Occupant* occupant);
	uint32_t BuildingType(cISC4Occupant* occupant);
	json BuildingCells(cISC4Occupant* occupant);

	struct BudgetItemInfo
	{
		uint32_t department = 0;
		uint32_t line = 0;
		uint32_t purpose = 0;
		int64_t cost = 0;
		float localFunding = 0.0f;
	};
	struct BuildingCost
	{
		int64_t monthly = 0;
		std::vector<BudgetItemInfo> items;
	};
	BuildingCost CostOf(cISC4BudgetSimulator* budget, cISC4Occupant* occupant);
	std::string DepartmentName(cISC4DepartmentBudget* department);

	// The building whose lot covers the cell, or nullptr; lot receives the lot (may be a lot without a building).
	cISC4Occupant* BuildingAt(cISC4City* city, int32_t x, int32_t z, cISC4Lot** lot = nullptr);

	const char* ZoneName(int32_t zone);
	// Zone name ("residential_low", "r_low", "R$"...) or number -> zone type, or -1.
	int32_t ParseZone(const json& value);

	// ------------------------------------------------------------------------------------------------- advisor data
	// The advisor Lua table `game`, which the game's advisor data pump fills every month (coverage, grades, capacities,
	// tax rates...). Numbers and strings are returned; tables and functions are skipped.
	struct GameTable
	{
		std::map<std::string, double> numbers;
		std::map<std::string, std::string> strings;
	};
	bool ReadGameTable(cISC4City* city, GameTable& out);
	// Fields whose name contains any of the words (lower case); all fields when words is empty.
	json GameFields(const GameTable& table, const std::vector<std::string>& words);

	// Localized LTEXT, or an empty string.
	std::string LocalizedText(uint32_t group, uint32_t instance);
}
