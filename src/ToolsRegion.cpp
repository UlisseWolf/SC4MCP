// SC4MCP - regions and the cities of a region: list them, switch region, open a city tile, go back to the region.
//
// From the 1.1.641 executable:
// - LoadRegion <name> (handler 0x007AF78C -> 0x007ADAC0) walks the region manager and compares <name> byte by byte
//   (case-sensitive) with each region's display name, the Name of its region.ini, not with the folder name. When the
//   region is already the current one it does nothing.
// - LoadCity <name> <size> <empty|full> (handler 0x007AF7DD -> 0x007AF4B0) matches a name only against established
//   cities; with an empty name it opens the first tile of the given size and state, so a given unestablished tile
//   cannot be chosen with it.
// - Clicking a tile (0x007ADC20) saves the region camera, then calls cISC4App::LoadCity(save file, tile) when the
//   tile has a save file, or cISC4App::RequestNewCity(tile) when it has none. load_city does the same for the tile
//   the client chooses by name or position, so established and unestablished tiles can both be opened.
#include "Tools.h"
#include "ToolsCommon.h"
#include "McpLog.h"

#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4Region.h"
#include "cISC4RegionalCity.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"
#include "SC4List.h"
#include "SC4String.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	std::string Text(cIGZString* text) { return text ? std::string(text->ToChar()) : std::string(); }

	std::string Trim(const std::string& text)
	{
		const size_t first = text.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) return {};
		const size_t last = text.find_last_not_of(" \t\r\n");
		return text.substr(first, last - first + 1);
	}

	// 1 = region view, 2 = city view, 0 = neither (loading, menus).
	int32_t View()
	{
		cISC4AppPtr app;
		if (!app) return 0;
		if (app->GetCity()) return 2;
		return app->GetRegion() ? 1 : 0;
	}

	const char* ViewName(int32_t view) { return view == 1 ? "region" : view == 2 ? "city" : "none"; }

	// ------------------------------------------------------------------------------------------------- regions

	struct RegionInfo
	{
		std::string folder;   // folder name under Regions
		std::string path;
		std::string name;     // Name in region.ini (what LoadRegion compares)
		int terrainType = -1;
		int cityFiles = 0;
		bool hasConfigBmp = false;
	};

	std::string RegionsFolder()
	{
		cISC4AppPtr app;
		cRZBaseString path;
		if (app && app->GetRegionsDirectory(path) && path.Strlen() > 0) return path.ToChar();
		return {};
	}

	// region.ini: [Regional Settings] Name = ..., Terrain type = ...
	void ReadRegionIni(const std::filesystem::path& file, RegionInfo& info)
	{
		std::ifstream in(file);
		std::string line;
		while (std::getline(in, line))
		{
			const size_t equals = line.find('=');
			if (equals == std::string::npos) continue;
			const std::string key = Lower(Trim(line.substr(0, equals)));
			const std::string value = Trim(line.substr(equals + 1));
			if (key == "name") info.name = value;
			else if (key == "terrain type")
			{
				try { info.terrainType = std::stoi(value); } catch (...) {}
			}
		}
	}

	std::vector<RegionInfo> ScanRegions(std::string& folder)
	{
		std::vector<RegionInfo> regions;
		folder = RegionsFolder();
		if (folder.empty()) return regions;
		std::error_code error;
		for (const auto& entry : std::filesystem::directory_iterator(folder, error))
		{
			if (!entry.is_directory(error)) continue;
			const std::filesystem::path ini = entry.path() / "region.ini";
			if (!std::filesystem::exists(ini, error)) continue;
			RegionInfo info;
			info.folder = entry.path().filename().string();
			info.path = entry.path().string();
			ReadRegionIni(ini, info);
			if (info.name.empty()) info.name = info.folder;
			info.hasConfigBmp = std::filesystem::exists(entry.path() / "config.bmp", error);
			for (const auto& file : std::filesystem::directory_iterator(entry.path(), error))
			{
				if (file.is_regular_file(error) && Lower(file.path().extension().string()) == ".sc4") ++info.cityFiles;
			}
			regions.push_back(info);
		}
		std::sort(regions.begin(), regions.end(), [](const RegionInfo& a, const RegionInfo& b) { return Lower(a.name) < Lower(b.name); });
		return regions;
	}

	json ListRegions()
	{
		std::string folder;
		const std::vector<RegionInfo> regions = ScanRegions(folder);
		if (folder.empty()) return Error("The game did not report its Regions folder.");
		cISC4AppPtr app;
		cISC4Region* current = app ? app->GetRegion() : nullptr;
		const std::string currentName = current ? Text(current->GetName()) : std::string();
		const std::string currentFolder = current ? Text(current->GetDirectoryName()) : std::string();

		json list = json::array();
		std::map<std::string, int> names;
		for (const RegionInfo& r : regions) ++names[r.name];
		for (const RegionInfo& r : regions)
		{
			json entry{ {"name", r.name}, {"folder", r.folder}, {"city_files", r.cityFiles}, {"has_config_bmp", r.hasConfigBmp} };
			if (r.terrainType >= 0) entry["terrain_type"] = r.terrainType;
			const bool isCurrent = !currentName.empty() && (r.name == currentName)
				&& (currentFolder.empty() || Lower(std::filesystem::path(currentFolder).filename().string()) == Lower(r.folder) || Lower(currentFolder) == Lower(r.folder));
			entry["current"] = isCurrent;
			if (names[r.name] > 1) entry["warning"] = "Another region has the same name: LoadRegion opens the first one the game lists.";
			list.push_back(entry);
		}
		json out{ {"regions_folder", folder}, {"count", list.size()}, {"regions", list}, {"view", ViewName(View())} };
		if (!currentName.empty()) out["current_region"] = currentName;
		out["note"] = "load_region switches region (from the region view); list_region_cities lists the city tiles of the current region.";
		return out;
	}

	// ------------------------------------------------------------------------------------------------- cities

	const char* SizeClass(int32_t sx, int32_t sz)
	{
		const int32_t size = std::max(sx, sz);
		if (size == 1 || size == 64) return "small";
		if (size == 2 || size == 128) return "medium";
		if (size == 4 || size == 256) return "large";
		return "unknown";
	}

	struct Tile
	{
		cISC4RegionalCity* city = nullptr;
		int32_t x = 0, z = 0, sx = 0, sz = 0;
		std::string name, saveFile;
		bool established = false;
	};

	std::vector<Tile> Tiles(cISC4Region* region)
	{
		std::vector<Tile> tiles;
		SC4List<cRZAutoRefCount<cISC4RegionalCity>> list;
		if (!region || !region->GetAllCities(list)) return tiles;
		for (const auto& entry : list)
		{
			cISC4RegionalCity* city = entry;
			if (!city) continue;
			Tile t;
			t.city = city;
			city->GetPosition(t.x, t.z);
			city->GetCitySize(t.sx, t.sz);
			cRZBaseString name, path;
			city->GetCityName(name);
			city->GetCitySaveFilePath(path);
			t.name = name.ToChar();
			t.saveFile = path.ToChar();
			t.established = city->GetEstablished();
			tiles.push_back(t);
		}
		std::sort(tiles.begin(), tiles.end(), [](const Tile& a, const Tile& b) { return a.z != b.z ? a.z < b.z : a.x < b.x; });
		return tiles;
	}

	json ListRegionCities(const json& arguments)
	{
		cISC4AppPtr app;
		cISC4Region* region = app ? app->GetRegion() : nullptr;
		if (!region) return Error("No region is loaded.");
		const bool onlyEstablished = arguments.value("established_only", false);
		cISC4RegionalCity* open = app->GetRegionalCity();
		json list = json::array();
		int established = 0;
		long long population = 0;
		for (const Tile& t : Tiles(region))
		{
			if (t.established) ++established;
			if (onlyEstablished && !t.established) continue;
			json c{ {"x", t.x}, {"z", t.z}, {"size", json{ {"x", t.sx}, {"z", t.sz} }}, {"size_class", SizeClass(t.sx, t.sz)},
				{"established", t.established}, {"has_save_file", !t.saveFile.empty()} };
			if (!t.name.empty()) c["name"] = t.name;
			if (!t.saveFile.empty()) c["save_file"] = std::filesystem::path(t.saveFile).filename().string();
			if (t.established)
			{
				cRZBaseString mayor;
				t.city->GetMayorName(mayor);
				if (mayor.Strlen() > 0) c["mayor"] = mayor.ToChar();
				c["population"] = t.city->GetPopulation();
				c["commercial_jobs"] = t.city->GetCommercialJobs();
				c["industrial_jobs"] = t.city->GetIndustrialJobs();
				population += t.city->GetPopulation();
			}
			if (open && t.city == open) c["open"] = true;
			list.push_back(c);
		}
		return json{ {"region", Text(region->GetName())}, {"folder", Text(region->GetDirectoryName())}, {"view", ViewName(View())},
			{"tiles", list.size()}, {"established", established}, {"population", population}, {"cities", list},
			{"note", "x and z are region tile positions. load_city opens a tile by name or by x and z, established or not."} };
	}

	// ------------------------------------------------------------------------------------------------- load_region

	json PlanLoadRegion(const json& arguments)
	{
		if (View() != 1) return Error(std::string("Switching region works in the region view; the game is in the ") + ViewName(View()) + " view. exit_to_region goes back to the region first.");
		const std::string wanted = arguments.value("region", std::string());
		if (wanted.empty()) return Error("Give region: its name or folder (list_regions).");
		std::string folder;
		const std::vector<RegionInfo> regions = ScanRegions(folder);
		const RegionInfo* match = nullptr;
		// Exact name, exact folder, then case-insensitive name or folder.
		for (const RegionInfo& r : regions) if (r.name == wanted) { match = &r; break; }
		if (!match) for (const RegionInfo& r : regions) if (r.folder == wanted) { match = &r; break; }
		if (!match)
		{
			std::vector<const RegionInfo*> found;
			for (const RegionInfo& r : regions) if (Lower(r.name) == Lower(wanted) || Lower(r.folder) == Lower(wanted)) found.push_back(&r);
			if (found.size() > 1) return Error("More than one region matches \"" + wanted + "\"; use the exact name.");
			if (!found.empty()) match = found[0];
		}
		if (!match) return Error("No region named \"" + wanted + "\" in " + folder + " (list_regions).");
		cISC4AppPtr app;
		cISC4Region* current = app ? app->GetRegion() : nullptr;
		json plan{ {"action", "load_region"}, {"region", match->name}, {"folder", match->folder}, {"city_files", match->cityFiles} };
		if (current && Text(current->GetName()) == match->name)
		{
			plan["already_current"] = true;
			plan["note"] = "This is already the current region: nothing to do.";
		}
		for (const RegionInfo& r : regions)
		{
			if (&r != match && r.name == match->name) { plan["warning"] = "Another region has the same name; the game opens the first one it lists."; break; }
		}
		// The command line: the name in double quotes (the game's tokenizer keeps it as one parameter).
		std::string clean;
		for (char c : match->name) if (c != '"') clean += c;
		plan["command"] = "LoadRegion \"" + clean + "\"";
		return plan;
	}

	json LoadRegion(const json& arguments)
	{
		json plan = PlanLoadRegion(arguments);
		if (plan.contains("error") || plan.value("already_current", false)) return plan;
		bool isError = false;
		json result = CallTool("run_game_command", json{ {"command", plan["command"]}, {"confirm", true} }, isError);
		plan["result"] = result;
		if (isError || result.contains("error")) { plan["error"] = "The game refused to switch region."; return plan; }
		plan["requested"] = true;
		plan["note"] = "The game loads the region on its next frames; call list_region_cities after a few seconds.";
		McpLog::Write("load_region: %s", plan["region"].get<std::string>().c_str());
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- load_city

	// Finds the tile by x and z, or by name (exact, then case-insensitive). Fills error when there is none or more
	// than one.
	bool FindTile(cISC4Region* region, const json& arguments, Tile& tile, std::string& error)
	{
		const std::vector<Tile> tiles = Tiles(region);
		if (arguments.contains("x") || arguments.contains("z"))
		{
			if (!arguments.contains("x") || !arguments.contains("z")) { error = "Give both x and z (region tile position from list_region_cities)."; return false; }
			const int32_t x = arguments["x"].get<int32_t>(), z = arguments["z"].get<int32_t>();
			// cSC4Region keeps every position a tile covers in its map (InsertCity, 0x004A4970), so GetCity also finds a
			// medium or large tile from any of its positions.
			cRZAutoRefCount<cISC4RegionalCity>* reference = x >= 0 && z >= 0 ? region->GetCity(static_cast<uint32_t>(x), static_cast<uint32_t>(z)) : nullptr;
			cISC4RegionalCity* city = reference ? static_cast<cISC4RegionalCity*>(*reference) : nullptr;
			for (const Tile& t : tiles)
			{
				if (city && t.city == city) { tile = t; return true; }
			}
			error = "No city tile at (" + std::to_string(x) + ", " + std::to_string(z) + ") (list_region_cities).";
			return false;
		}
		const std::string wanted = arguments.value("name", arguments.value("city", std::string()));
		if (wanted.empty()) { error = "Give name, or x and z (list_region_cities)."; return false; }
		std::vector<const Tile*> found;
		for (const Tile& t : tiles) if (t.name == wanted) found.push_back(&t);
		if (found.empty()) for (const Tile& t : tiles) if (Lower(t.name) == Lower(wanted)) found.push_back(&t);
		if (found.empty()) { error = "No city named \"" + wanted + "\" in this region (list_region_cities)."; return false; }
		if (found.size() > 1)
		{
			error = "More than one tile is named \"" + wanted + "\":";
			for (const Tile* t : found) error += " (" + std::to_string(t->x) + ", " + std::to_string(t->z) + ")";
			error += ". Use x and z.";
			return false;
		}
		tile = *found[0];
		return true;
	}

	json PlanLoadCity(const json& arguments)
	{
		if (View() != 1) return Error(std::string("load_city works in the region view; the game is in the ") + ViewName(View()) + " view. exit_to_region goes back to the region first.");
		cISC4AppPtr app;
		cISC4Region* region = app->GetRegion();
		Tile tile;
		std::string error;
		if (!FindTile(region, arguments, tile, error)) return Error(error);
		json plan{ {"action", "load_city"}, {"region", Text(region->GetName())}, {"x", tile.x}, {"z", tile.z}, {"size_class", SizeClass(tile.sx, tile.sz)},
			{"established", tile.established}, {"has_save_file", !tile.saveFile.empty()} };
		if (!tile.name.empty()) plan["name"] = tile.name;
		plan["how"] = tile.saveFile.empty() ? "new city on an empty tile (cISC4App::RequestNewCity)" : "open the saved city (cISC4App::LoadCity)";
		return plan;
	}

	json LoadCity(const json& arguments)
	{
		json plan = PlanLoadCity(arguments);
		if (plan.contains("error")) return plan;
		cISC4AppPtr app;
		cISC4Region* region = app->GetRegion();
		cRZAutoRefCount<cISC4RegionalCity>* reference = region->GetCity(static_cast<uint32_t>(plan["x"].get<int32_t>()), static_cast<uint32_t>(plan["z"].get<int32_t>()));
		cISC4RegionalCity* city = reference ? static_cast<cISC4RegionalCity*>(*reference) : nullptr;
		if (!city) return Error("The region has no tile object at that position.");
		SC4String path;
		city->GetCitySaveFilePath(path);
		bool ok = false;
		if (path.Strlen() > 0) ok = app->LoadCity(path, reference);
		else ok = app->RequestNewCity(reference);
		McpLog::Write("load_city: (%d, %d) %s %s", plan["x"].get<int>(), plan["z"].get<int>(), path.Strlen() > 0 ? "load" : "new", ok ? "ok" : "refused");
		plan["requested"] = ok;
		if (!ok) plan["error"] = "The game refused to open the tile.";
		else plan["note"] = "The city opens on the next frames; the city tools answer once it has loaded (get_game_info shows the view).";
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- exit_to_region

	json PlanExitToRegion(const json& arguments)
	{
		cISC4AppPtr app;
		if (!app || !app->GetCity()) return Error("No city is open.");
		cRZBaseString name;
		app->GetCity()->GetCityName(name);
		return json{ {"action", "exit_to_region"}, {"city", name.ToChar()}, {"save", arguments.value("save", true)}, {"show_dialog", arguments.value("show_dialog", false)} };
	}

	json ExitToRegion(const json& arguments)
	{
		json plan = PlanExitToRegion(arguments);
		if (plan.contains("error")) return plan;
		cISC4AppPtr app;
		const bool save = plan["save"].get<bool>(), dialog = plan["show_dialog"].get<bool>();
		bool saved = true;
		if (save && !dialog) saved = app->RequestSaveCity(false, false);
		const bool requested = app->RequestGoToRegionView(dialog);
		McpLog::Write("exit_to_region: save %d (%s), dialog %d, %s", save ? 1 : 0, saved ? "requested" : "refused", dialog ? 1 : 0, requested ? "requested" : "refused");
		plan["save_requested"] = save && !dialog ? json(saved) : json(nullptr);
		plan["requested"] = requested;
		if (!requested) plan["error"] = "The game refused to go back to the region.";
		else plan["note"] = dialog ? "The game asks the player whether to save." : "The game saves (when asked) and returns to the region view on its next frames.";
		return plan;
	}

	json WithConfirm(json properties)
	{
		properties["dry_run"] = json{ {"type", "boolean"}, {"description", "Only describe the change, do not apply it."} };
		properties["confirm"] = json{ {"type", "boolean"}, {"description", "Apply without asking (the user has already agreed)."} };
		return properties;
	}
}

namespace ToolsCommon
{
	void AddRegionTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "list_regions",
			"The regions in the game's Regions folder: name (from region.ini, the name the game uses), folder, number of city files, and which one is loaded now. Works in the region and city views.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return ListRegions(); }) });

		tools.push_back({ "list_region_cities",
			"Every city tile of the current region, established or not: region position x and z, size (small, medium, large), name, mayor, population and jobs, whether it has a save file, and which one is open. established_only=true lists only established cities. Works in the region and city views.",
			Schema({ {"established_only", {{"type", "boolean"}}} }), Permission::Read, GameThread([](const json& arguments) { return ListRegionCities(arguments); }) });

		tools.push_back({ "load_region",
			"From the region view, switches to another region by its name or folder (list_regions), as the game's LoadRegion command, which needs the exact name of region.ini. Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"region", {{"type", "string"}}} }), json::array({ "region" })), Permission::Control,
			Guarded(PlanLoadRegion, [](const json& a) { return LoadRegion(a); }, [](const json& p) { return "Switch to the region \"" + p.value("region", std::string()) + "\"?"; }) });

		tools.push_back({ "load_city",
			"From the region view, opens a city tile of the current region, chosen by name or by its region position x and z (list_region_cities), as clicking it: a tile with a save file opens the saved city, an empty tile starts a new city. Works for unestablished tiles too, unlike the game's LoadCity command. Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"name", {{"type", "string"}}}, {"x", {{"type", "integer"}}}, {"z", {{"type", "integer"}}} })), Permission::Control,
			Guarded(PlanLoadCity, [](const json& a) { return LoadCity(a); },
				[](const json& p)
				{
					const std::string what = p.contains("name") ? "\"" + p["name"].get<std::string>() + "\"" : std::string("the tile");
					return "Open " + what + " at (" + std::to_string(p.value("x", 0)) + ", " + std::to_string(p.value("z", 0)) + ")" + (p.value("has_save_file", false) ? "?" : " as a new city?");
				}) });

		tools.push_back({ "exit_to_region",
			"Closes the open city and goes back to the region view. save=true (default) saves the city first; show_dialog=true lets the game ask the player instead. Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"save", {{"type", "boolean"}}}, {"show_dialog", {{"type", "boolean"}}} })), Permission::Control,
			Guarded(PlanExitToRegion, [](const json& a) { return ExitToRegion(a); },
				[](const json& p) { return "Close " + p.value("city", std::string("the city")) + (p.value("save", true) ? " (saving it first)" : " WITHOUT saving") + " and go back to the region?"; }) });
	}
}
