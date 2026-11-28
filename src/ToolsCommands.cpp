// SC4MCP - game commands.
//
// SimCity 4 has a command system that it exposes to Lua test scripts, to the -NetCommandGenerator socket (port 50020)
// and to the cheat box: GetAppState, GetCellAltitude, PlaceLot, PlaceNetwork, Demolish, SetViewTarget, TakeSnapshot...
// (documented in https://github.com/0xC0000054/sc4-game-command-documentation). The commands are registered with the
// framework's command server (cIGZCommandServer), so a DLL can run them directly, without the socket and without any
// command line argument: the string is turned into a command ID and a parameter set (ConvertStringToCommand), run
// (ExecuteCommand) and its output turned back into text (ConvertCommandOutputToString).
//
// Every command is classified before it runs (read, view, control, build, modal, dangerous, forbidden; commands of
// other DLL plugins are "unknown") and each class needs the matching SC4MCP.ini permission. The curated tools
// (place_lot, place_network, demolish, set_terrain...) validate their arguments and preview the change first.
#include "Tools.h"
#include "ToolsCommon.h"
#include "McpLog.h"

#include "cIGZCommandParameterSet.h"
#include "cIGZCommandServer.h"
#include "cIGZVariant.h"
#include "cISC4FloraSimulator.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4Lot.h"
#include "cISC4LotConfiguration.h"
#include "cISC4LotConfigurationManager.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "cISC4ZoneManager.h"
#include "cRZBaseString.h"
#include "GZServPtrs.h"
#include "SC4Vector.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using nlohmann::json;
using namespace ToolsCommon;

namespace
{
	// ------------------------------------------------------------------------------------------------- classes

	enum class CommandClass
	{
		Read,       // returns information, changes nothing
		View,       // camera, overlays, UI tools: changes what the player sees, not the city
		Control,    // simulation speed, saving, names, loading a city from the region
		Build,      // changes the city: lots, networks, demolition, terrain, flora
		Modal,      // opens a modal dialog: the game thread waits for the player
		Dangerous,  // quits, crashes, writes files at any path, runs scripts, opens a browser, blocks input
		Forbidden,  // never run (CreateException)
		Unknown     // not in the Maxis documentation, usually registered by another DLL plugin
	};

	const char* ClassName(CommandClass kind)
	{
		switch (kind)
		{
		case CommandClass::Read: return "read";
		case CommandClass::View: return "view";
		case CommandClass::Control: return "control";
		case CommandClass::Build: return "build";
		case CommandClass::Modal: return "modal";
		case CommandClass::Dangerous: return "dangerous";
		case CommandClass::Forbidden: return "forbidden";
		default: return "unknown";
		}
	}

	struct KnownCommand
	{
		const char* name;
		CommandClass kind;
		const char* scope;   // app (city and region), city, region
		const char* summary;
	};

	using C = CommandClass;

	// The Maxis commands of sc4-game-command-documentation (AppCommands.md, CityCommands.md, RegionCommands.md).
	const std::vector<KnownCommand>& KnownCommands()
	{
		static const std::vector<KnownCommand> commands = {
			// ---- App: miscellaneous
			{ "Assert", C::Read, "app", "Unimplemented in retail builds." },
			{ "CancelWaitForMessage2", C::Dangerous, "app", "Test-script message wait; meaningless outside a Lua script." },
			{ "CreateException", C::Forbidden, "app", "Crashes the game on purpose." },
			{ "CreateGZLog", C::Dangerous, "app", "Creates an empty file at any path." },
			{ "ExecuteCheat", C::Unknown, "app", "Runs a cheat code (run_cheat does the same)." },
			{ "ExecuteScript", C::Dangerous, "app", "Runs a script file or resource." },
			{ "GameDelay", C::Control, "app", "Delays the game: GameDelay frames|time <n>." },
			{ "GamePause", C::Control, "app", "Pauses or resumes: GamePause <bool> <All|Draw|Animation|SimulationClock|24HourClock>." },
			{ "GetAppState", C::Read, "app", "1 = region view, 2 = city view." },
			{ "GetDateAndTime", C::Read, "app", "Real date and/or time: GetDateAndTime [date] [time]." },
			{ "GetDirectory", C::Read, "app", "Path of a game folder: GetDirectory <App|Data|Plugin|UserPlugin|Region|UserData|MySim|Album|Temp|TestScripts|Desktop...>." },
			{ "GetFrameCount", C::Read, "app", "Frame count." },
			{ "GetFrameRate", C::Read, "app", "Frame rate." },
			{ "GetMemInfo", C::Read, "app", "Memory use in KB: GetMemInfo [ram] [ram_max] [disk] [disk_max] [faults] [malloc]." },
			{ "GetOccupantCount", C::Read, "app", "Occupant count (undocumented parameters)." },
			{ "GetPathDirectory", C::Read, "app", "Strips the file name from a path." },
			{ "GetPathFileName", C::Read, "app", "Strips path and extension from a file name." },
			{ "GetPopupModalDialogsEnabled", C::Read, "app", "Whether popup modal dialogs are enabled." },
			{ "GetPopupModalDialogsSafe", C::Read, "app", "Whether it is safe to show a modal dialog." },
			{ "GetRandomInteger", C::Read, "app", "Random integer: GetRandomInteger <min> <max>." },
			{ "GetRandomNumber", C::Read, "app", "Random float: GetRandomNumber <min> <max>." },
			{ "GetTimerTime", C::Read, "app", "Timer time stamp: GetTimerTime <ns|us|ms|sec|min>." },
			{ "GZLog", C::Read, "app", "Unimplemented in retail builds." },
			{ "ListCommands", C::Read, "app", "Unimplemented in retail builds (list_game_commands lists them)." },
			{ "ReadRegistry", C::Read, "app", "Unimplemented in retail builds." },
			{ "RZCheckHeap", C::Read, "app", "Unimplemented in retail builds." },
			{ "SendMessage", C::Dangerous, "app", "Broadcasts a message on the game's internal messaging system." },
			{ "SetDebugLevel", C::Dangerous, "app", "Framework debug level (no effect in retail builds)." },
			{ "SetGZLogLevel", C::Read, "app", "Unimplemented in retail builds." },
			{ "SetPopupModalDialogsEnabled", C::Control, "app", "Enables or disables popup modal dialogs: SetPopupModalDialogsEnabled <bool>." },
			{ "SetScriptAutoYield", C::Dangerous, "app", "Test-script auto yield; meaningless outside a Lua script." },
			{ "TakeSnapshot", C::Dangerous, "app", "Screenshot to any path: TakeSnapshot <file> <x> <y> <width> <height> (width and height are required; format from the extension). take_snapshot writes to SC4MCP_screenshots." },
			{ "ViewWebBrowser", C::Dangerous, "app", "Opens a web browser at a URL." },
			{ "WaitForMessage2", C::Dangerous, "app", "Test-script message wait; meaningless outside a Lua script." },
			{ "WriteRegistry", C::Dangerous, "app", "Unimplemented in retail builds." },
			// ---- App: tools
			{ "Demolish", C::Build, "app", "Demolishes rectangles: Demolish x1 y1 x2 y2 [more rectangles] [Sim] [None|Explode]." },
			{ "GetViewTarget", C::Read, "app", "Camera position x y z." },
			{ "PlaceLot", C::Build, "app", "Places a lot: PlaceLot <lot IID> <cell x> <cell z> <facing 0-3> <plop effects> <bool>." },
			{ "PlaceNetwork", C::Build, "app", "Places a network: PlaceNetwork <type 0-12> <x1> <z1> <x2> <z2>." },
			{ "PlaceNetworkIntersection", C::Read, "app", "Unimplemented." },
			{ "PlaceZone", C::Build, "app", "Zones a rectangle: PlaceZone <zone 0-15> <x1> <z1> <x2> <z2>." },
			{ "SetViewTarget", C::View, "app", "Moves the camera: SetViewTarget cell <x> <z> | position <x> <y> <z>." },
			// ---- App: windows
			{ "GZWinClickButton", C::Dangerous, "app", "Clicks a button of a window." },
			{ "GZWinCreateWindow", C::Dangerous, "app", "Creates a scripted window." },
			{ "GZWinDestroyWindow", C::Dangerous, "app", "Destroys a window." },
			{ "GZWinEnableUserInput", C::Dangerous, "app", "Disables or enables the player's input." },
			{ "GZWinGenerateInputEvent", C::Dangerous, "app", "Generates an input event." },
			{ "GZWinIsWindowPresent", C::Read, "app", "Status 0 when the window is present: GZWinIsWindowPresent <id> <parent id>." },
			{ "GZWinMessageBox", C::Modal, "app", "Shows a message box and returns the button clicked." },
			{ "GZWinMoveCursorToPosition", C::Dangerous, "app", "Moves the mouse cursor." },
			{ "GZWinMoveCursorToWindow", C::Dangerous, "app", "Moves the mouse cursor to a window." },
			{ "GZWinMoveWindow", C::Dangerous, "app", "Moves a window." },
			{ "GZWinSelectListboxItem", C::Dangerous, "app", "Selects a list box item." },
			{ "GZWinSetWindowText", C::Dangerous, "app", "Sets the text of a window." },
			{ "GZWinShowWindow", C::Dangerous, "app", "Shows or hides a window." },
			{ "GZWinWaitForWindow", C::Dangerous, "app", "Waits for a window (blocking when asked)." },
			// ---- City
			{ "AnimationRecorder", C::Dangerous, "city", "Animation recorder." },
			{ "ApplyTerrainBrush", C::Build, "city", "Applies a terrain brush." },
			{ "Cancel", C::View, "app", "Cancels the current tool or selection." },
			{ "CenterOnCursor", C::View, "city", "Centres the view on the cursor." },
			{ "DemolishSignsAndLabels", C::Build, "city", "Removes every sign and label." },
			{ "DispatchFireTool", C::View, "city", "Selects the fire dispatch tool." },
			{ "DispatchPoliceTool", C::View, "city", "Selects the police dispatch tool." },
			{ "EnableDayAndNight", C::View, "city", "Day and night cycle." },
			{ "EnableDayOnly", C::View, "city", "Always day." },
			{ "EnableNightOnly", C::View, "city", "Always night." },
			{ "FullScreenRefresh", C::View, "city", "Redraws the screen." },
			{ "GetAltitude", C::Read, "city", "Altitude at a position: GetAltitude <x> <z> (metres)." },
			{ "GetCellAltitude", C::Read, "city", "Altitude of a cell: GetCellAltitude <cell x> <cell z>." },
			{ "GetCityEstablished", C::Read, "city", "Whether the city is established (mayor mode)." },
			{ "GetCityName", C::Read, "city", "City name." },
			{ "GetCitySize", C::Read, "city", "Cells in x and z." },
			{ "GetMayorName", C::Read, "city", "Mayor name." },
			{ "GetSeaLevel", C::Read, "city", "Sea level." },
			{ "GetSimulationDate", C::Read, "city", "Simulation date parts: GetSimulationDate year month \"day of month\" \"day of week\" \"day of year\" number." },
			{ "OpenAudioOptionsDialog", C::Modal, "city", "Opens the audio options." },
			{ "OpenCheatCodeDialog", C::Modal, "city", "Opens the cheat box." },
			{ "OpenGraphicOptionsDialog", C::Modal, "city", "Opens the graphic options." },
			{ "OpenObliterateCityDialog", C::Dangerous, "city", "Opens the dialog that destroys the city." },
			{ "OpenPhotoAlbumDialog", C::Modal, "city", "Opens the photo album." },
			{ "OpenPlayOptionsDialog", C::Modal, "city", "Opens the play options." },
			{ "OpenSnapshotDialog", C::Modal, "city", "Opens the snapshot dialog." },
			{ "PauseAnimation", C::View, "city", "Pauses the animations." },
			{ "PlaceBuilding", C::Build, "city", "Places a building: PlaceBuilding <building id> <x> <z> (world position)." },
			{ "PlaceFlora", C::Build, "city", "Broken in 1.1.641: the handler puts its z argument in the altitude and z = -1, off the map. place_flora calls the flora simulator directly." },
			{ "PlaceLotTool", C::View, "city", "Selects the place-lot tool for a lot: PlaceLotTool <lot IID>." },
			{ "QuitCity", C::Dangerous, "city", "Exits to the region: QuitCity <show confirmation>." },
			{ "QuitGame", C::Dangerous, "app", "Exits the game." },
			{ "RotateCCW", C::View, "city", "Rotates the view counter-clockwise." },
			{ "RotateCW", C::View, "city", "Rotates the view clockwise." },
			{ "RotateLotCCW", C::View, "city", "Rotates the lot being placed counter-clockwise." },
			{ "RotateLotCW", C::View, "city", "Rotates the lot being placed clockwise." },
			{ "SaveCity", C::Control, "city", "Saves the city: SaveCity [suppress confirmation]." },
			{ "SaveCityQuick", C::Control, "city", "Saves without updating the region thumbnails." },
			{ "SetAltitude", C::Build, "city", "Altitude at a position: SetAltitude <x> <z> <altitude>." },
			{ "SetCellAltitude", C::Build, "city", "Altitude of cells: SetCellAltitude <x1> <z1> [x2] [z2] <altitude>." },
			{ "SetCityEstablished", C::Build, "city", "Makes the city established (mayor mode)." },
			{ "SetCityName", C::Control, "city", "Sets the city name." },
			{ "SetExpandedToolTips", C::View, "app", "Expanded tool tips on or off." },
			{ "SetMayorName", C::Control, "city", "Sets the mayor name." },
			{ "SetSeaLevel", C::Build, "city", "Sets the sea level." },
			{ "SimSpeedFast", C::Control, "city", "Simulation speed fast." },
			{ "SimSpeedMedium", C::Control, "city", "Simulation speed medium." },
			{ "SimSpeedPause", C::Control, "city", "Pauses the simulation." },
			{ "SimSpeedSlow", C::Control, "city", "Simulation speed slow." },
			{ "StepAnimation", C::View, "city", "Steps the animations." },
			{ "TextDecal", C::Build, "city", "Places a text decal." },
			{ "ToggleGodPanel", C::View, "city", "Shows or hides the god panel." },
			{ "ToggleGridVisibility", C::View, "city", "Shows or hides the grid." },
			{ "ToggleMayorPanel", C::View, "city", "Shows or hides the mayor panel." },
			{ "ToggleMySimPanel", C::View, "city", "Shows or hides the MySim panel." },
			{ "ToggleOptionsPanel", C::View, "city", "Shows or hides the options panel." },
			{ "ToggleSignsAndLabels", C::View, "city", "Shows or hides signs and labels." },
			{ "ToggleTerrainContourDisplay", C::View, "city", "Shows or hides the terrain contours." },
			{ "ToggleUIVisibility", C::View, "city", "Shows or hides the user interface." },
			{ "ToxicSpill", C::Build, "city", "Starts a toxic spill disaster." },
			{ "Zoom0", C::View, "city", "Zoom level 0 (farthest)." }, { "Zoom1", C::View, "city", "Zoom level 1." },
			{ "Zoom2", C::View, "city", "Zoom level 2." }, { "Zoom3", C::View, "city", "Zoom level 3." },
			{ "Zoom4", C::View, "city", "Zoom level 4." }, { "Zoom5", C::View, "city", "Zoom level 5 (closest)." },
			{ "ZoomIn", C::View, "city", "Zooms in." },
			{ "ZoomOut", C::View, "city", "Zooms out." },
			// ---- Region
			{ "LoadCity", C::Control, "region", "Opens a city: LoadCity <name> <small|medium|large|any> <empty|full>." },
			{ "LoadRegion", C::Control, "region", "Opens a region: LoadRegion <name>." },
			{ "RegionBitmapLoad", C::Modal, "region", "Opens a file browser for the region bitmap." },
		};
		return commands;
	}

	// UI tool selectors (RoadTool, ZoneLowResidentialTool, PlaceHospitalTool, ScrollLeft...): only select a tool or
	// scroll, the player still has to click.
	bool IsUiSelector(const std::string& lower)
	{
		auto ends = [&](const char* suffix)
		{
			const size_t n = std::char_traits<char>::length(suffix);
			return lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0;
		};
		return ends("tool") || lower.rfind("scroll", 0) == 0;
	}

	const KnownCommand* FindKnown(const std::string& name)
	{
		const std::string lower = Lower(name);
		for (const KnownCommand& command : KnownCommands())
		{
			if (Lower(command.name) == lower) return &command;
		}
		return nullptr;
	}

	CommandClass Classify(const std::string& name)
	{
		if (const KnownCommand* known = FindKnown(name)) return known->kind;
		if (IsUiSelector(Lower(name))) return CommandClass::View;
		return CommandClass::Unknown;
	}

	// The permission each class needs (SC4MCP.ini).
	bool ClassAllowed(CommandClass kind, std::string& why)
	{
		switch (kind)
		{
		case C::Read:
		case C::View: return true;
		case C::Control:
		case C::Modal:
			if (PermissionAllowed(Permission::Control)) return true;
			why = "needs AllowControl=1 in SC4MCP.ini";
			return false;
		case C::Build:
			if (PermissionAllowed(Permission::Build)) return true;
			why = "needs AllowBuild=1 in SC4MCP.ini";
			return false;
		case C::Unknown:
			if (PermissionAllowed(Permission::Cheats)) return true;
			why = "commands registered by other DLL plugins need AllowCheats=1 in SC4MCP.ini";
			return false;
		case C::Dangerous:
			if (PermissionAllowed(Permission::Dangerous)) return true;
			why = "needs AllowDangerousCommands=1 in SC4MCP.ini";
			return false;
		default:
			why = "this command is never run by SC4MCP";
			return false;
		}
	}

	// ------------------------------------------------------------------------------------------------- text

	std::string FirstToken(const std::string& line)
	{
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string::npos) return {};
		const size_t end = line.find_first_of(" \t", start);
		return line.substr(start, end == std::string::npos ? std::string::npos : end - start);
	}

	// A string argument in double quotes (the command parser splits on spaces); quotes and line breaks are dropped.
	std::string Quote(const std::string& text)
	{
		std::string clean;
		for (char c : text)
		{
			if (c != '"' && c != '\r' && c != '\n') clean += c;
		}
		return "\"" + clean + "\"";
	}

	std::string Num(double value)
	{
		char text[48];
		std::snprintf(text, sizeof(text), "%.4f", value);
		return text;
	}

	// The output text split into values: numbers become numbers, "quoted strings" lose their quotes.
	json ParseOutput(const std::string& text)
	{
		json values = json::array();
		size_t i = 0;
		while (i < text.size())
		{
			while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
			if (i >= text.size()) break;
			std::string token;
			if (text[i] == '"')
			{
				const size_t close = text.find('"', i + 1);
				token = text.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
				i = close == std::string::npos ? text.size() : close + 1;
				values.push_back(token);
				continue;
			}
			const size_t end = text.find_first_of(" \t\r\n", i);
			token = text.substr(i, end == std::string::npos ? std::string::npos : end - i);
			i = end == std::string::npos ? text.size() : end;
			char* rest = nullptr;
			const double number = std::strtod(token.c_str(), &rest);
			if (rest && *rest == '\0' && !token.empty())
			{
				if (number == static_cast<double>(static_cast<int64_t>(number)) && token.find_first_of(".eE") == std::string::npos) values.push_back(static_cast<int64_t>(number));
				else values.push_back(number);
			}
			else if (Lower(token) == "true" || Lower(token) == "false") values.push_back(Lower(token) == "true");
			else values.push_back(token);
		}
		return values;
	}

	// ------------------------------------------------------------------------------------------------- executor

	// Runs one command line on the game thread (the caller is already there). Returns { command, id, return_code,
	// status, output, values } or { error }.
	json Execute(const std::string& rawLine)
	{
		const size_t first = rawLine.find_first_not_of(" \t");
		const size_t last = rawLine.find_last_not_of(" \t\r\n");
		const std::string line = first == std::string::npos ? std::string() : rawLine.substr(first, last - first + 1);
		cIGZCommandServerPtr server;
		if (!server) return Error("The game's command server is unavailable.");
		const std::string name = FirstToken(line);
		if (name.empty()) return Error("Empty command.");
		const uint32_t length = static_cast<uint32_t>(line.size());

		if (server->GetCommandIDFromCommandString(line.c_str(), static_cast<int32_t>(length)) == 0)
		{
			const KnownCommand* known = FindKnown(name);
			std::string message = "The game has no command \"" + name + "\" registered now.";
			if (known && std::string(known->scope) == "city") message += " It is a city command: open a city first.";
			else if (known && std::string(known->scope) == "region") message += " It is a region command: go to the region view first.";
			else if (!known) message += " list_game_commands lists the registered commands.";
			return Error(message);
		}

		// Same sequence as the game's own string ExecuteCommand (0x00929994 in 1.1.641): ConvertStringToCommand
		// splits the line on spaces and tabs (a "quoted string" stays one parameter, without its quotes; no escapes),
		// looks up the command by its first word, and always allocates a new parameter set (reference count 1) with
		// the remaining words as string variants under IDs 0..n-1; it overwrites the pointer it is given, so it is
		// passed null and released once here. The variants convert on demand: t/f/true/false, 0x hex, decimal,
		// negative and floating-point numbers are all accepted.
		cIGZCommandParameterSet* input = nullptr;
		uint32_t id = 0;
		if (!server->ConvertStringToCommand(line.c_str(), length, id, input) || id == 0 || !input)
		{
			if (input) input->Release();
			return Error("The game could not parse the command line \"" + line + "\".");
		}

		cIGZCommandParameterSet* output = nullptr;
		if (!server->CreateCommandParameterSet(&output) || !output)
		{
			input->Release();
			return Error("Could not create a command parameter set.");
		}

		// The dispatcher sends the command as a synchronous message and returns the status parameter of the output
		// (0x2B999D2B); when the handler sets none, it sets 0. Handlers set 2 when they fail; a few commands use the
		// status as their answer (GZWinIsWindowPresent: 0 = present).
		const int32_t returnCode = server->ExecuteCommand(id, input, output);

		json out{ {"command", line}, {"id", Hex(id)}, {"return_code", returnCode}, {"ok", returnCode == 0} };
		bool hasStatus = false;
		if (cIGZVariant* status = output->GetParameter(cIGZCommandParameterSet::kStatusParameterID)) // null when absent
		{
			int32_t value = 0;
			if (status->GetValSint32(value)) out["status"] = value;
			hasStatus = true;
		}
		// The output text lists every output parameter in ID order, joined by spaces (values with spaces are quoted).
		// The status parameter has the highest ID, so it is the last value ("GetAppState" answers "1 0"): it is
		// reported as status and removed from values.
		cRZBaseString text;
		json values = json::array();
		if (server->ConvertCommandOutputToString(output, text) && text.Strlen() > 0)
		{
			out["output"] = text.ToChar();
			values = ParseOutput(text.ToChar());
			if (hasStatus && !values.empty()) values.erase(values.size() - 1);
		}
		else
		{
			out["output"] = "";
		}
		out["values"] = values;
		output->Release();
		input->Release();
		McpLog::Write("game command: %s -> %d", line.c_str(), returnCode);
		return out;
	}

	// Runs a command and returns its values, or an empty array when it failed.
	json Values(const std::string& line)
	{
		json result = Execute(line);
		return result.contains("values") ? result["values"] : json::array();
	}

	bool FirstNumber(const std::string& line, double& value)
	{
		const json values = Values(line);
		if (values.empty() || !values[0].is_number()) return false;
		value = values[0].get<double>();
		return true;
	}

	int32_t AppState()
	{
		double state = 0;
		return FirstNumber("GetAppState", state) ? static_cast<int32_t>(state) : 0;
	}

	const char* AppStateName(int32_t state)
	{
		return state == 1 ? "region" : state == 2 ? "city" : "unknown";
	}

	// ------------------------------------------------------------------------------------------------- generic tools

	json ListGameCommands(const json& arguments)
	{
		cIGZCommandServerPtr server;
		if (!server) return Error("The game's command server is unavailable.");
		const std::string contains = Lower(arguments.value("contains", std::string()));
		const std::string onlyClass = Lower(arguments.value("class", std::string()));

		json list = json::array();
		std::set<std::string> seen;
		auto add = [&](const std::string& name, const std::string& parameters, bool registered)
		{
			if (name.empty() || !seen.insert(Lower(name)).second) return;
			const CommandClass kind = Classify(name);
			if (!contains.empty() && Lower(name).find(contains) == std::string::npos) return;
			if (!onlyClass.empty() && onlyClass != ClassName(kind)) return;
			json entry{ {"name", name}, {"class", ClassName(kind)}, {"registered", registered} };
			if (!parameters.empty()) entry["parameter_info"] = parameters;
			if (const KnownCommand* known = FindKnown(name))
			{
				entry["scope"] = known->scope;
				entry["summary"] = known->summary;
			}
			else
			{
				entry["documented"] = false;
			}
			std::string why;
			entry["allowed"] = ClassAllowed(kind, why);
			if (!why.empty()) entry["why_not"] = why;
			list.push_back(entry);
		};

		SC4Vector<uint32_t> ids;
		const bool enumerated = server->EnumerateCommands(ids);
		if (enumerated)
		{
			for (uint32_t id : ids)
			{
				cRZBaseString name, parameters;
				if (server->GetCommandInfo(id, name, parameters)) add(name.ToChar(), parameters.ToChar(), true);
			}
		}
		// The documented commands that are not registered in this view.
		if (arguments.value("include_unregistered", false) || !enumerated)
		{
			for (const KnownCommand& known : KnownCommands())
			{
				cRZBaseString name(known.name);
				add(known.name, std::string(), server->GetCommandID(name) != 0);
			}
		}
		std::sort(list.begin(), list.end(), [](const json& a, const json& b) { return Lower(a["name"].get<std::string>()) < Lower(b["name"].get<std::string>()); });
		const int32_t state = AppState();
		return json{ {"view", AppStateName(state)}, {"count", list.size()}, {"commands", list},
			{"classes", "read and view: always allowed (query_game_command); control and modal: AllowControl; build: AllowBuild; unknown (other DLL plugins): AllowCheats; dangerous: AllowDangerousCommands; forbidden: never."} };
	}

	// Classifies a command line; fills plan with the class and an error when it may not run.
	json PlanCommand(const std::string& line)
	{
		const std::string name = FirstToken(line);
		if (name.empty()) return Error("command is empty.");
		if (line.find_first_of("\r\n") != std::string::npos) return Error("command must be one line.");
		const CommandClass kind = Classify(name);
		json plan{ {"action", "run_game_command"}, {"command", line}, {"name", name}, {"class", ClassName(kind)} };
		if (const KnownCommand* known = FindKnown(name)) plan["summary"] = known->summary;
		std::string why;
		if (!ClassAllowed(kind, why)) plan["error"] = "\"" + name + "\" is a " + ClassName(kind) + " command: " + why + ".";
		if (kind == C::Modal) plan["warning"] = "This command opens a modal dialog: the game waits for the player and SC4MCP's other tools time out until the dialog is closed.";
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- game info

	json GetGameInfo()
	{
		const int32_t state = AppState();
		json out{ {"app_state", state}, {"view", AppStateName(state)} };
		const json dateTime = Values("GetDateAndTime");
		if (dateTime.size() >= 2) out["real_date_time"] = json{ {"date", dateTime[0]}, {"time", dateTime[1]} };
		double number = 0;
		if (FirstNumber("GetFrameRate", number)) out["frame_rate"] = number;
		if (FirstNumber("GetFrameCount", number)) out["frame_count"] = number;
		const json memory = Values("GetMemInfo ram ram_max disk disk_max faults");
		if (memory.size() >= 5) out["memory_kb"] = json{ {"ram", memory[0]}, {"ram_max", memory[1]}, {"page_file", memory[2]}, {"page_file_max", memory[3]}, {"page_faults", memory[4]} };

		json folders = json::object();
		for (const char* folder : { "App", "Data", "Plugin", "UserData", "UserPlugin", "Region", "MySim", "Album", "Temp", "TestScripts" })
		{
			const json values = Values(std::string("GetDirectory ") + folder);
			if (!values.empty() && values[0].is_string()) folders[folder] = values[0];
		}
		out["folders"] = folders;
		const json view = Values("GetViewTarget");
		if (view.size() >= 3) out["camera"] = json{ {"x", view[0]}, {"y", view[1]}, {"z", view[2]} };

		if (state == 2)
		{
			json city = json::object();
			const json name = Values("GetCityName");
			if (!name.empty()) city["name"] = name[0];
			const json mayor = Values("GetMayorName");
			if (!mayor.empty()) city["mayor"] = mayor[0];
			const json established = Values("GetCityEstablished");
			if (!established.empty()) city["established"] = established[0];
			const json size = Values("GetCitySize");
			if (size.size() >= 2) city["size_cells"] = json{ {"x", size[0]}, {"z", size[1]} };
			if (FirstNumber("GetSeaLevel", number)) city["sea_level"] = number;
			const json date = Values("GetSimulationDate year month \"day of month\" \"day of week\" \"day of year\" number");
			if (date.size() >= 6) city["simulation_date"] = json{ {"year", date[0]}, {"month", date[1]}, {"day", date[2]}, {"day_of_week", date[3]}, {"day_of_year", date[4]}, {"internal", date[5]} };
			out["city"] = city;
		}
		return out;
	}

	// ------------------------------------------------------------------------------------------------- terrain

	struct Rect { int32_t x1, z1, x2, z2; };

	// Reads x1, z1, x2, z2 (x2/z2 default to x1/z1), ordered and checked against the city and a cell limit.
	json ReadRect(cISC4City* city, const json& arguments, Rect& rect, int32_t maxCells)
	{
		rect.x1 = arguments.at("x1").get<int32_t>();
		rect.z1 = arguments.at("z1").get<int32_t>();
		rect.x2 = arguments.value("x2", rect.x1);
		rect.z2 = arguments.value("z2", rect.z1);
		if (rect.x1 > rect.x2) std::swap(rect.x1, rect.x2);
		if (rect.z1 > rect.z2) std::swap(rect.z1, rect.z2);
		if (!InCity(city, rect.x1, rect.z1) || !InCity(city, rect.x2, rect.z2)) return Error("The rectangle is outside the city.");
		if (maxCells > 0 && (rect.x2 - rect.x1 + 1) * (rect.z2 - rect.z1 + 1) > maxCells) return Error("The rectangle has more than " + std::to_string(maxCells) + " cells.");
		return json::object();
	}

	json RectJson(const Rect& r) { return json{ {"x1", r.x1}, {"z1", r.z1}, {"x2", r.x2}, {"z2", r.z2} }; }

	// Developed lots in a rectangle, with the plopped (zone 15) ones flagged.
	json LotsIn(cISC4City* city, const Rect& r, size_t& count, size_t& plopped)
	{
		count = plopped = 0;
		json list = json::array();
		cISC4LotManager* lots = city->GetLotManager();
		cISC4ZoneManager* zones = city->GetZoneManager();
		if (!lots) return list;
		std::set<cISC4Lot*> seen;
		for (int32_t x = r.x1; x <= r.x2; ++x)
		{
			for (int32_t z = r.z1; z <= r.z2; ++z)
			{
				cISC4Lot* lot = lots->GetLot(x, z, false);
				if (!lot || !seen.insert(lot).second) continue;
				++count;
				const bool isPlopped = zones && static_cast<int32_t>(zones->GetZoneType(x, z)) == 15;
				if (isPlopped) ++plopped;
				if (list.size() < 30)
				{
					cISC4Occupant* building = BuildingAt(city, x, z);
					list.push_back(json{ {"x", x}, {"z", z}, {"building", building ? BuildingName(building) : std::string("(empty lot)")}, {"plopped", isPlopped} });
				}
			}
		}
		return list;
	}

	json GetTerrain(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		Rect r{};
		json check = ReadRect(city, arguments, r, 0);
		if (check.contains("error")) return check;
		const int32_t step = std::clamp(arguments.value("step", 1), 1, 64);
		const int32_t samplesX = (r.x2 - r.x1) / step + 1, samplesZ = (r.z2 - r.z1) / step + 1;
		if (static_cast<int64_t>(samplesX) * samplesZ > 4096) return Error("More than 4096 samples: raise step or use a smaller rectangle.");

		double sea = 0;
		const bool haveSea = FirstNumber("GetSeaLevel", sea);
		double low = 1e30, high = -1e30, sum = 0;
		int64_t samples = 0, underWater = 0;
		json grid = json::array();
		const bool withGrid = samplesX * samplesZ <= 1024;
		json lowest, highest;
		for (int32_t z = r.z1; z <= r.z2; z += step)
		{
			json row = json::array();
			for (int32_t x = r.x1; x <= r.x2; x += step)
			{
				double altitude = 0;
				if (!FirstNumber("GetCellAltitude " + std::to_string(x) + " " + std::to_string(z), altitude))
				{
					row.push_back(nullptr);
					continue;
				}
				++samples;
				sum += altitude;
				if (altitude < low) { low = altitude; lowest = json{ {"x", x}, {"z", z} }; }
				if (altitude > high) { high = altitude; highest = json{ {"x", x}, {"z", z} }; }
				if (haveSea && altitude < sea) ++underWater;
				row.push_back(std::round(altitude * 100.0) / 100.0);
			}
			if (withGrid) grid.push_back(row);
		}
		if (samples == 0) return Error("The game returned no altitude (GetCellAltitude is a city command).");
		json out{ {"rect", RectJson(r)}, {"step", step}, {"samples", samples}, {"min", low}, {"min_cell", lowest}, {"max", high}, {"max_cell", highest},
			{"average", sum / static_cast<double>(samples)} };
		if (haveSea)
		{
			out["sea_level"] = sea;
			out["samples_under_water"] = underWater;
		}
		if (withGrid) out["grid"] = grid; // rows by z, columns by x
		else out["note"] = "More than 1024 samples: no grid. Raise step to get one.";
		return out;
	}

	json PlanTerrain(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		json plan{ {"action", "set_terrain"} };
		double sea = 0;
		const bool haveSea = FirstNumber("GetSeaLevel", sea);
		if (haveSea) plan["current_sea_level"] = sea;
		if (arguments.contains("sea_level"))
		{
			const double level = arguments["sea_level"].get<double>();
			if (level < 0 || level > 2000) return Error("sea_level must be between 0 and 2000 metres.");
			plan["new_sea_level"] = level;
			plan["command"] = "SetSeaLevel " + Num(level);
			return plan;
		}
		if (!arguments.contains("altitude")) return Error("Give altitude (with x1, z1 and optionally x2, z2) or sea_level.");
		const double altitude = arguments["altitude"].get<double>();
		if (altitude < 0 || altitude > 5000) return Error("altitude must be between 0 and 5000 metres.");
		Rect r{};
		json check = ReadRect(city, arguments, r, 64 * 64);
		if (check.contains("error")) return check;
		double low = 1e30, high = -1e30;
		for (const auto& [x, z] : { std::pair{ r.x1, r.z1 }, std::pair{ r.x2, r.z1 }, std::pair{ r.x1, r.z2 }, std::pair{ r.x2, r.z2 } })
		{
			double value = 0;
			if (FirstNumber("GetCellAltitude " + std::to_string(x) + " " + std::to_string(z), value)) { low = std::min(low, value); high = std::max(high, value); }
		}
		size_t lots = 0, plopped = 0;
		plan["rect"] = RectJson(r);
		plan["cells"] = (r.x2 - r.x1 + 1) * (r.z2 - r.z1 + 1);
		if (low <= high) plan["current_altitude_corners"] = json{ {"min", low}, {"max", high} };
		plan["new_altitude"] = altitude;
		plan["lots"] = LotsIn(city, r, lots, plopped);
		plan["lots_affected"] = lots;
		if (haveSea && altitude < sea) plan["warning"] = "The new altitude is below the sea level: the cells will be under water.";
		plan["command"] = "SetCellAltitude " + std::to_string(r.x1) + " " + std::to_string(r.z1) + " " + std::to_string(r.x2) + " " + std::to_string(r.z2) + " " + Num(altitude);
		if (lots > 0 && !arguments.value("allow_demolition", false))
		{
			plan["error"] = "The rectangle contains " + std::to_string(lots) + " lot(s); changing the terrain under them can destroy them. Pass allow_demolition=true to do it anyway.";
		}
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- lots

	int32_t ParseFacing(const json& value)
	{
		if (value.is_number_integer()) return value.get<int32_t>();
		const std::string text = Lower(value.get<std::string>());
		if (text == "north" || text == "n") return 0;
		if (text == "east" || text == "e") return 1;
		if (text == "south" || text == "s") return 2;
		if (text == "west" || text == "w") return 3;
		return -1;
	}

	json PlanLot(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		const uint32_t iid = ParseId(arguments.at("lot"));
		const int32_t x = arguments.at("x").get<int32_t>(), z = arguments.at("z").get<int32_t>();
		const int32_t facing = arguments.contains("facing") ? ParseFacing(arguments["facing"]) : 2;
		if (facing < 0 || facing > 3) return Error("facing must be north, east, south, west or 0-3.");
		if (!InCity(city, x, z)) return Error("The cell is outside the city.");
		static const char* kFacing[] = { "north", "east", "south", "west" };

		json plan{ {"action", "place_lot"}, {"lot", Hex(iid)}, {"x", x}, {"z", z}, {"facing", kFacing[facing]} };
		cISC4LotConfigurationManager* manager = city->GetLotConfigurationManager();
		cISC4LotConfiguration* config = manager ? manager->GetLotConfiguration(iid) : nullptr;
		if (!config) return Error("No lot configuration with instance " + Hex(iid) + " is loaded (get_building_properties or read_resource show exemplars).");
		cRZBaseString name;
		if (config->GetName(name)) plan["name"] = name.ToChar();
		uint32_t sizeX = 0, sizeZ = 0;
		if (config->GetSize(sizeX, sizeZ))
		{
			plan["size"] = json{ {"x", sizeX}, {"z", sizeZ} };
			// East and west facings turn the lot by 90 degrees.
			const int32_t spanX = static_cast<int32_t>(facing % 2 ? sizeZ : sizeX), spanZ = static_cast<int32_t>(facing % 2 ? sizeX : sizeZ);
			Rect r{ x, z, x + std::max(spanX, 1) - 1, z + std::max(spanZ, 1) - 1 };
			plan["estimated_rect"] = RectJson(r);
			if (!InCity(city, r.x2, r.z2)) plan["warning"] = "The lot may extend past the edge of the city from this cell; the game will refuse it.";
			else
			{
				size_t lots = 0, plopped = 0;
				json occupied = LotsIn(city, r, lots, plopped);
				if (lots > 0)
				{
					plan["occupied_by"] = occupied;
					plan["warning"] = "Cells of the estimated rectangle already have lots: the game may refuse the lot or replace what is there.";
				}
			}
		}
		const bool effects = arguments.value("plop_effects", true);
		plan["plop_effects"] = effects;
		// Numbers in decimal: the command parser reads plain integers.
		plan["command"] = "PlaceLot " + std::to_string(iid) + " " + std::to_string(x) + " " + std::to_string(z) + " " + std::to_string(facing) + (effects ? " true" : " false") + " true";
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- networks

	const char* kNetworks[] = { "road", "rail", "highway", "street", "water_pipe", "power_line", "avenue", "subway", "light_rail", "monorail", "one_way_road", "dirt_road", "ground_highway" };

	int32_t ParseNetwork(const json& value)
	{
		if (value.is_number_integer()) return value.get<int32_t>();
		std::string text = Lower(value.get<std::string>());
		std::replace(text.begin(), text.end(), ' ', '_');
		std::replace(text.begin(), text.end(), '-', '_');
		if (text == "pipe" || text == "pipes" || text == "water") return 4;
		if (text == "power" || text == "power_pole" || text == "power_lines") return 5;
		if (text == "oneway" || text == "one_way") return 10;
		if (text == "lightrail" || text == "el_rail" || text == "elevated_rail") return 8;
		if (text == "dirt" || text == "rhw") return 11;
		for (int32_t i = 0; i < 13; ++i)
		{
			if (text == kNetworks[i]) return i;
		}
		return -1;
	}

	json PlanNetwork(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		const int32_t type = ParseNetwork(arguments.at("network"));
		if (type < 0 || type > 12) return Error("network must be road, rail, highway, street, water_pipe, power_line, avenue, subway, light_rail, monorail, one_way_road, dirt_road, ground_highway or 0-12.");
		const int32_t x1 = arguments.at("x1").get<int32_t>(), z1 = arguments.at("z1").get<int32_t>();
		const int32_t x2 = arguments.at("x2").get<int32_t>(), z2 = arguments.at("z2").get<int32_t>();
		if (!InCity(city, x1, z1) || !InCity(city, x2, z2)) return Error("Both ends must be inside the city.");
		const int32_t length = std::max(std::abs(x2 - x1), std::abs(z2 - z1)) + 1;
		if (length > 256) return Error("At most 256 cells per call.");
		json plan{ {"action", "place_network"}, {"network", kNetworks[type]}, {"type", type}, {"from", json{ {"x", x1}, {"z", z1} }}, {"to", json{ {"x", x2}, {"z", z2} }},
			{"length_cells", length} };
		if (x1 != x2 && z1 != z2 && std::abs(x2 - x1) != std::abs(z2 - z1)) plan["note"] = "The ends are not on a straight or diagonal line: the game draws its own path between them, as when dragging the tool.";
		plan["command"] = "PlaceNetwork " + std::to_string(type) + " " + std::to_string(x1) + " " + std::to_string(z1) + " " + std::to_string(x2) + " " + std::to_string(z2);
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- demolition

	json PlanDemolish(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		Rect r{};
		json check = ReadRect(city, arguments, r, 64 * 64);
		if (check.contains("error")) return check;
		const std::string effect = Lower(arguments.value("effect", std::string("default")));
		if (effect != "default" && effect != "none" && effect != "explode") return Error("effect must be default, none or explode.");
		size_t lots = 0, plopped = 0;
		json plan{ {"action", "demolish"}, {"rect", RectJson(r)}, {"cells", (r.x2 - r.x1 + 1) * (r.z2 - r.z1 + 1)}, {"effect", effect} };
		plan["lots"] = LotsIn(city, r, lots, plopped);
		plan["lots_demolished"] = lots;
		plan["plopped_lots"] = plopped;
		plan["note"] = "Networks (roads, rails, pipes, power lines) and flora in the rectangle are demolished too.";
		std::string command = "Demolish " + std::to_string(r.x1) + " " + std::to_string(r.z1) + " " + std::to_string(r.x2) + " " + std::to_string(r.z2);
		if (effect == "none") command += " None";
		else if (effect == "explode") command += " Explode";
		plan["command"] = command;
		if (plopped > 0 && !arguments.value("allow_plopped", false))
		{
			plan["error"] = "The rectangle contains " + std::to_string(plopped) + " plopped building(s) (civic, utility, reward, landmark). Pass allow_plopped=true to demolish them too.";
		}
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- flora

	json PlanFlora(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		const uint32_t id = ParseId(arguments.at("flora"));
		const int32_t x = arguments.at("x").get<int32_t>(), z = arguments.at("z").get<int32_t>();
		if (!InCity(city, x, z)) return Error("The cell is outside the city.");
		cISC4FloraSimulator* flora = city->GetFloraSimulator();
		if (!flora) return Error("Flora simulator unavailable.");
		if (!flora->IsFloraTypeValid(id)) return Error("No flora type " + Hex(id) + " is loaded.");
		// World positions are in metres; a city cell is 16 m. The flora goes at the centre of the cell.
		const double wx = x * 16.0 + 8.0, wz = z * 16.0 + 8.0;
		return json{ {"action", "place_flora"}, {"flora", Hex(id)}, {"x", x}, {"z", z}, {"world_x", wx}, {"world_z", wz} };
	}

	// The PlaceFlora game command is broken in 1.1.641 (handler at 0x006B3497 builds the position as
	// {x, z, -1.0}, while the flora simulator reads {x, y, z} with y <= 0 meaning "on the terrain"), so the flora is
	// added through cISC4FloraSimulator::AddNewFloraOccupant with the position the simulator expects.
	json ApplyFlora(const json& arguments)
	{
		json plan = PlanFlora(arguments);
		if (plan.contains("error")) return plan;
		cISC4FloraSimulator* flora = City()->GetFloraSimulator();
		float position[3] = { plan["world_x"].get<float>(), -1.0f, plan["world_z"].get<float>() };
		cISC4Occupant* occupant = flora->AddNewFloraOccupant(ParseId(arguments.at("flora")), position, false);
		McpLog::Write("place_flora: %s at (%d, %d) %s", plan["flora"].get<std::string>().c_str(), plan["x"].get<int>(), plan["z"].get<int>(), occupant ? "placed" : "refused");
		if (!occupant) return Error("The flora simulator refused the place (water, a lot or a network on the cell?).");
		RecordChange("Place flora " + plan["flora"].get<std::string>() + " at (" + std::to_string(plan["x"].get<int>()) + ", " + std::to_string(plan["z"].get<int>()) + ")", json::array());
		plan["applied"] = true;
		return plan;
	}

	// ------------------------------------------------------------------------------------------------- city info

	json PlanCityInfo(const json& arguments)
	{
		if (!City()) return NoCity();
		if (!arguments.contains("city_name") && !arguments.contains("mayor_name")) return Error("Give city_name and/or mayor_name.");
		json plan{ {"action", "set_city_info"} };
		json commands = json::array();
		auto add = [&](const char* key, const char* get, const char* set)
		{
			if (!arguments.contains(key)) return;
			const std::string value = arguments[key].get<std::string>();
			if (value.empty() || value.size() > 64) throw std::invalid_argument(std::string(key) + " must be 1 to 64 characters.");
			const json current = Values(get);
			plan[std::string("current_") + key] = current.empty() ? json(nullptr) : current[0];
			plan[std::string("new_") + key] = value;
			commands.push_back(std::string(set) + " " + Quote(value));
		};
		add("city_name", "GetCityName", "SetCityName");
		add("mayor_name", "GetMayorName", "SetMayorName");
		plan["commands"] = commands;
		return plan;
	}

	json PlanLoadCity(const json& arguments)
	{
		const int32_t state = AppState();
		if (state != 1) return Error(std::string("load_city works in the region view; the game is in the ") + AppStateName(state) + " view.");
		const std::string name = arguments.value("city", std::string());
		std::string size = Lower(arguments.value("size", std::string("any")));
		if (size != "small" && size != "medium" && size != "large" && size != "any") return Error("size must be small, medium, large or any.");
		const bool established = arguments.value("established", true);
		return json{ {"action", "load_city"}, {"city", name.empty() ? json("(first city found)") : json(name)}, {"size", size}, {"established", established},
			{"command", "LoadCity " + Quote(name) + " " + size + (established ? " full" : " empty")} };
	}

	// ------------------------------------------------------------------------------------------------- apply

	// Runs the command (or commands) of a plan and records the change.
	json ApplyPlan(const json& plan, const std::string& description, bool record)
	{
		if (plan.contains("error")) return plan;
		json results = json::array();
		std::vector<std::string> lines;
		if (plan.contains("commands")) for (const json& c : plan["commands"]) lines.push_back(c.get<std::string>());
		else lines.push_back(plan.at("command").get<std::string>());
		for (const std::string& line : lines)
		{
			json result = Execute(line);
			if (result.contains("error")) return result;
			results.push_back(result);
		}
		if (record) RecordChange(description, json::array());
		json out = plan;
		out["applied"] = true;
		out["results"] = results;
		return out;
	}

	std::string Describe(const json& plan)
	{
		const std::string action = plan.value("action", std::string());
		if (action == "place_lot") return "Place lot " + plan.value("name", plan.value("lot", std::string())) + " at (" + std::to_string(plan.value("x", 0)) + ", " + std::to_string(plan.value("z", 0)) + ") facing " + plan.value("facing", std::string());
		if (action == "place_network")
		{
			return "Place " + plan.value("network", std::string()) + " from (" + std::to_string(plan["from"].value("x", 0)) + ", " + std::to_string(plan["from"].value("z", 0)) + ") to ("
				+ std::to_string(plan["to"].value("x", 0)) + ", " + std::to_string(plan["to"].value("z", 0)) + ")";
		}
		if (action == "demolish")
		{
			std::string text = "Demolish " + std::to_string(plan.value("cells", 0)) + " cells";
			const size_t lots = plan.value("lots_demolished", static_cast<size_t>(0));
			if (lots) text += " with " + std::to_string(lots) + " lot(s)";
			return text;
		}
		if (action == "place_flora") return "Place flora " + plan.value("flora", std::string()) + " at (" + std::to_string(plan.value("x", 0)) + ", " + std::to_string(plan.value("z", 0)) + ")";
		if (action == "set_terrain")
		{
			if (plan.contains("new_sea_level")) return "Set the sea level to " + Num(plan["new_sea_level"].get<double>()) + " m";
			return "Set the altitude of " + std::to_string(plan.value("cells", 0)) + " cells to " + Num(plan["new_altitude"].get<double>()) + " m";
		}
		if (action == "set_city_info")
		{
			std::string text = "Rename";
			if (plan.contains("new_city_name")) text += " the city to \"" + plan["new_city_name"].get<std::string>() + "\"";
			if (plan.contains("new_mayor_name")) text += std::string(plan.contains("new_city_name") ? " and" : "") + " the mayor to \"" + plan["new_mayor_name"].get<std::string>() + "\"";
			return text;
		}
		if (action == "load_city") return "Open the city " + plan["city"].dump() + " (the region view closes)";
		return "Run the game command \"" + plan.value("command", std::string()) + "\" (" + plan.value("class", std::string()) + ")";
	}

	// ------------------------------------------------------------------------------------------------- snapshot

	// Client size of the game's main window: the largest visible top-level window of this process.
	bool GameWindowSize(int32_t& width, int32_t& height)
	{
#ifdef _WIN32
		struct Finder { DWORD process; LONG area; LONG width; LONG height; } finder{ GetCurrentProcessId(), 0, 0, 0 };
		EnumWindows([](HWND window, LPARAM parameter) -> BOOL
		{
			Finder& f = *reinterpret_cast<Finder*>(parameter);
			DWORD process = 0;
			GetWindowThreadProcessId(window, &process);
			if (process != f.process || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
			RECT rect{};
			if (!GetClientRect(window, &rect)) return TRUE;
			const LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
			if (w * h > f.area) { f.area = w * h; f.width = w; f.height = h; }
			return TRUE;
		}, reinterpret_cast<LPARAM>(&finder));
		width = finder.width;
		height = finder.height;
		return finder.area > 0;
#else
		width = height = 0;
		return false;
#endif
	}

	json TakeSnapshot(const json& arguments, bool& isError)
	{
		const std::string folder = DataFolder("SC4MCP_screenshots");
		char stamp[32];
		const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		std::tm local{};
#ifdef _WIN32
		localtime_s(&local, &now);
#else
		localtime_r(&now, &local);
#endif
		std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
		const std::string format = Lower(arguments.value("format", std::string("png")));
		if (format != "png" && format != "jpg" && format != "bmp") { isError = true; return Error("format must be png, jpg or bmp."); }
		const std::filesystem::path file = std::filesystem::path(folder) / (std::string("snapshot_") + stamp + "." + format);
		std::error_code ignored;
		std::filesystem::remove(file, ignored);

		// TakeSnapshot <file> <x> <y> <width> <height> (handler at 0x005C8F2A, capture at 0x005F9560): the four numbers
		// are x, y, width and height, not two corners, and the command does nothing when width or height is 0, so the
		// size of the game window is always passed. The image is captured and written within the command; the format
		// comes from the file extension (Gimex codec chosen by name).
		json result = OnGameThread([&]() -> json
		{
			int32_t x = 0, y = 0, width = 0, height = 0;
			if (arguments.contains("crop"))
			{
				const json& crop = arguments["crop"];
				x = crop.value("x", 0);
				y = crop.value("y", 0);
				width = crop.value("width", 0);
				height = crop.value("height", 0);
			}
			if (width <= 0 || height <= 0)
			{
				int32_t windowWidth = 0, windowHeight = 0;
				if (!GameWindowSize(windowWidth, windowHeight)) return Error("Could not find the size of the game window; pass crop = {x, y, width, height}.");
				if (width <= 0) width = windowWidth - x;
				if (height <= 0) height = windowHeight - y;
			}
			if (x < 0 || y < 0 || width <= 0 || height <= 0) return Error("crop must have x, y >= 0 and a positive width and height.");
			json r = Execute("TakeSnapshot " + Quote(file.generic_string()) + " " + std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(width) + " " + std::to_string(height));
			r["rect"] = json{ {"x", x}, {"y", y}, {"width", width}, {"height", height} };
			return r;
		}, isError);
		if (isError) return result;

		std::error_code error;
		const uintmax_t size = std::filesystem::exists(file, error) ? std::filesystem::file_size(file, error) : 0;
		json out{ {"file", file.string()}, {"bytes", size}, {"command", result} };
		if (size == 0)
		{
			out["note"] = "The game did not write the file (status " + std::to_string(result.value("return_code", -1)) + "): the window may be minimized or the rectangle outside it.";
			return out;
		}
		std::ifstream in(file, std::ios::binary);
		std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::string mime;
		if (data.size() > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') mime = "image/png";
		else if (data.size() > 3 && data[0] == 0xFF && data[1] == 0xD8) mime = "image/jpeg";
		else if (data.size() > 2 && data[0] == 'B' && data[1] == 'M') mime = "image/bmp";
		out["format"] = mime.empty() ? "unknown" : mime;
		if ((mime == "image/png" || mime == "image/jpeg") && data.size() <= 4 * 1024 * 1024 && arguments.value("return_image", true))
		{
			out["_image"] = json{ {"data", Base64Encode(data)}, {"mimeType", mime} };
		}
		else if (arguments.value("return_image", true))
		{
			out["note"] = "The image is not returned inline (BMP, unknown format or larger than 4 MB); it is in the file.";
		}
		return out;
	}

	// ------------------------------------------------------------------------------------------------- camera

	json SetCamera(const json& arguments)
	{
		json done = json::array();
		if (arguments.contains("x") || arguments.contains("z"))
		{
			cISC4City* city = City();
			if (!city) return NoCity();
			const int32_t x = arguments.at("x").get<int32_t>(), z = arguments.at("z").get<int32_t>();
			if (!InCity(city, x, z)) return Error("The cell is outside the city.");
			json r = Execute("SetViewTarget cell " + std::to_string(x) + " " + std::to_string(z));
			if (r.contains("error")) return r;
			done.push_back(r["command"]);
		}
		if (arguments.contains("zoom"))
		{
			const int32_t zoom = arguments["zoom"].get<int32_t>();
			if (zoom < 0 || zoom > 5) return Error("zoom must be 0 (farthest) to 5 (closest), the game's Zoom0 to Zoom5 commands.");
			json r = Execute("Zoom" + std::to_string(zoom));
			if (r.contains("error")) return r;
			done.push_back(r["command"]);
		}
		if (arguments.contains("rotate"))
		{
			const int32_t turns = arguments["rotate"].get<int32_t>();
			if (turns < -3 || turns > 3) return Error("rotate must be -3 to 3 quarter turns (positive = clockwise).");
			for (int32_t i = 0; i < std::abs(turns); ++i)
			{
				json r = Execute(turns > 0 ? "RotateCW" : "RotateCCW");
				if (r.contains("error")) return r;
				done.push_back(r["command"]);
			}
		}
		for (const char* toggle : { "grid", "signs_and_labels", "terrain_contours", "ui" })
		{
			if (!arguments.contains(toggle)) continue;
			// The game only toggles these; SC4MCP does not know the current state, so "toggle": true flips it.
			if (!arguments[toggle].get<bool>()) continue;
			const std::string command = std::string(toggle) == "grid" ? "ToggleGridVisibility" : std::string(toggle) == "signs_and_labels" ? "ToggleSignsAndLabels"
				: std::string(toggle) == "terrain_contours" ? "ToggleTerrainContourDisplay" : "ToggleUIVisibility";
			json r = Execute(command);
			if (r.contains("error")) return r;
			done.push_back(r["command"]);
		}
		if (arguments.contains("lighting"))
		{
			const std::string lighting = Lower(arguments["lighting"].get<std::string>());
			const char* command = lighting == "day" ? "EnableDayOnly" : lighting == "night" ? "EnableNightOnly" : lighting == "cycle" ? "EnableDayAndNight" : nullptr;
			if (!command) return Error("lighting must be day, night or cycle.");
			json r = Execute(command);
			if (r.contains("error")) return r;
			done.push_back(r["command"]);
		}
		json out{ {"commands", done} };
		const json view = Values("GetViewTarget");
		if (view.size() >= 3) out["camera"] = json{ {"x", view[0]}, {"y", view[1]}, {"z", view[2]} };
		return out;
	}

	// dry_run and confirm, as the other tools that change the city.
	json WithConfirm(json properties)
	{
		properties["dry_run"] = json{ {"type", "boolean"}, {"description", "Only describe the change, do not apply it."} };
		properties["confirm"] = json{ {"type", "boolean"}, {"description", "Apply without asking (the user has already agreed)."} };
		return properties;
	}

	// A guarded tool whose apply step runs the plan's command(s).
	Handler PlanTool(std::function<json(const json&)> plan, bool record = true)
	{
		return Guarded(plan,
			[plan, record](const json& arguments) { const json p = plan(arguments); return ApplyPlan(p, Describe(p), record); },
			[](const json& p) { return Describe(p) + "?"; });
	}
}

namespace ToolsCommon
{
	void AddCommandTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "list_game_commands",
			"The game commands registered now (the command system of Lua test scripts and the -NetCommandGenerator socket), including those of other DLL plugins, with their parameter info, SC4MCP class (read, view, control, build, modal, dangerous, forbidden, unknown), a summary for the documented Maxis commands and whether SC4MCP's settings allow them. City commands are registered only in the city view, region commands only in the region view. Filters: contains, class; include_unregistered=true adds the documented commands missing in this view.",
			Schema({ {"contains", {{"type", "string"}}}, {"class", {{"type", "string"}}}, {"include_unregistered", {{"type", "boolean"}}} }), Permission::Read,
			GameThread([](const json& arguments) { return ListGameCommands(arguments); }) });

		tools.push_back({ "query_game_command",
			"Runs a read or view game command and returns its output text and parsed values, e.g. \"GetCellAltitude 10 20\", \"GetDirectory UserPlugin\", \"GetSimulationDate year month\", \"SetViewTarget cell 64 64\", \"Zoom3\", \"ToggleGridVisibility\". Commands that change the city are refused here: use run_game_command or a dedicated tool.",
			Schema({ {"command", {{"type", "string"}, {"description", "The command line, e.g. \"GetCellAltitude 10 20\"."}}} }, json::array({ "command" })), Permission::Read,
			GameThread([](const json& arguments) -> json
			{
				const std::string line = arguments.at("command").get<std::string>();
				json plan = PlanCommand(line);
				if (plan.contains("error")) return plan;
				const std::string kind = plan["class"].get<std::string>();
				if (kind != "read" && kind != "view") return Error("\"" + plan["name"].get<std::string>() + "\" is a " + kind + " command: use run_game_command (it asks for confirmation).");
				return Execute(line);
			}) });

		// Read and view commands run at once; the others need their SC4MCP.ini permission and a confirmation.
		static const Handler guardedCommand = Guarded(
			[](const json& arguments) { return PlanCommand(arguments.at("command").get<std::string>()); },
			[](const json& arguments) -> json
			{
				const json plan = PlanCommand(arguments.at("command").get<std::string>());
				if (plan.contains("error")) return plan;
				json result = Execute(plan["command"].get<std::string>());
				const std::string kind = plan["class"].get<std::string>();
				if (!result.contains("error") && (kind == "build" || kind == "unknown" || kind == "dangerous")) RecordChange("Game command: " + plan["command"].get<std::string>(), json::array());
				result["class"] = kind;
				return result;
			},
			[](const json& p)
			{
				std::string text = Describe(p) + "?";
				if (p.contains("summary")) text += " " + p["summary"].get<std::string>();
				if (p.contains("warning")) text += " " + p["warning"].get<std::string>();
				return text;
			});
		tools.push_back({ "run_game_command",
			"Runs any game command line (see list_game_commands and https://github.com/0xC0000054/sc4-game-command-documentation), e.g. \"PlaceLot 0x6534284a 40 40 2 true true\", \"PlaceNetwork 0 10 10 10 40\", \"Demolish 5 5 8 8\", \"SaveCityQuick true\", or a command of another DLL plugin. Each command class needs its SC4MCP.ini permission: control and modal AllowControl, build AllowBuild, commands of other DLL plugins AllowCheats, dangerous AllowDangerousCommands; CreateException is never run. Read and view commands run at once; the others ask for confirmation (dry_run=true only classifies). Prefer the dedicated tools (place_lot, place_network, demolish, set_terrain...) which check their arguments first.",
			Schema(WithConfirm({ {"command", {{"type", "string"}}} }), json::array({ "command" })), Permission::Control,
			[](const json& arguments, bool& isError, const Tools::Context& context) -> json
			{
				const std::string line = arguments.at("command").get<std::string>();
				const CommandClass kind = Classify(FirstToken(line));
				if ((kind == C::Read || kind == C::View) && !arguments.value("dry_run", false))
				{
					return OnGameThread([&] { return Execute(line); }, isError);
				}
				return guardedCommand(arguments, isError, context);
			} });

		tools.push_back({ "get_game_info",
			"Game-level information from the game commands: view (region or city), real date and time, frame rate and count, memory use, the game folders (App, Data, Plugin, UserData, UserPlugin, Region, MySim, Album, Temp, TestScripts), camera position and, in a city, name, mayor, established, size, sea level and the simulation date. Works in the region view too.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetGameInfo(); }) });

		tools.push_back({ "get_terrain",
			"Terrain altitude (metres) of a cell or a rectangle of cells: min, max, average, the cells under the sea level and, up to 1024 samples, the grid (rows by z, columns by x). step samples every N cells; at most 4096 samples.",
			Schema({ {"x1", {{"type", "integer"}}}, {"z1", {{"type", "integer"}}}, {"x2", {{"type", "integer"}}}, {"z2", {{"type", "integer"}}},
				{"step", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}} }, json::array({ "x1", "z1" })), Permission::Read,
			GameThread([](const json& arguments) { return GetTerrain(arguments); }) });

		tools.push_back({ "set_camera",
			"Moves the game camera and changes the view, without changing the city: x and z centre the view on a cell, zoom 0 (farthest) to 5 (closest), rotate -3..3 quarter turns (positive clockwise), lighting day/night/cycle, and grid, signs_and_labels, terrain_contours, ui = true to toggle those overlays. Returns the camera position. Useful before take_snapshot.",
			Schema({ {"x", {{"type", "integer"}}}, {"z", {{"type", "integer"}}}, {"zoom", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5}}},
				{"rotate", {{"type", "integer"}, {"minimum", -3}, {"maximum", 3}}}, {"lighting", {{"type", "string"}}},
				{"grid", {{"type", "boolean"}}}, {"signs_and_labels", {{"type", "boolean"}}}, {"terrain_contours", {{"type", "boolean"}}}, {"ui", {{"type", "boolean"}}} }), Permission::Read,
			GameThread([](const json& arguments) { return SetCamera(arguments); }) });

		tools.push_back({ "take_snapshot",
			"Takes a screenshot of the game window (as the game's TakeSnapshot command) into SC4MCP_screenshots next to the DLL and returns it as an image the assistant can look at (PNG or JPEG up to 4 MB). crop = {x, y, width, height} in window pixels (default the whole window); format png (default), jpg or bmp; return_image=false returns only the file path. set_camera first to frame the view.",
			Schema({ {"format", {{"type", "string"}}}, {"crop", {{"type", "object"}}}, {"return_image", {{"type", "boolean"}}} }), Permission::Read,
			[](const json& arguments, bool& isError, const Tools::Context&) { return TakeSnapshot(arguments, isError); } });

		tools.push_back({ "place_lot",
			"Places a lot by the instance ID of its lot configuration exemplar (as the game's PlaceLot command): x and z are the cell of its corner, facing north/east/south/west (default south), plop_effects (default true). Checks that the lot is loaded and previews its size, name and the cells it would cover; the game charges the plop cost and may refuse the place. Asks for confirmation; dry_run=true only previews. Cannot be undone.",
			Schema(WithConfirm({ {"lot", {{"type", "string"}, {"description", "Lot configuration exemplar instance ID, e.g. \"0x6534284a\"."}}}, {"x", {{"type", "integer"}}}, {"z", {{"type", "integer"}}},
				{"facing", {{"type", "string"}}}, {"plop_effects", {{"type", "boolean"}}} }), json::array({ "lot", "x", "z" })), Permission::Build,
			PlanTool(PlanLot) });

		tools.push_back({ "place_network",
			"Draws a network between two cells, as dragging the game's tool (PlaceNetwork): network is road, rail, highway, street, water_pipe, power_line, avenue, subway, light_rail, monorail, one_way_road, dirt_road (the RHW base) or ground_highway, or its number 0-12. At most 256 cells long. The game charges the cost and may refuse the path. Asks for confirmation; dry_run=true only previews. Cannot be undone.",
			Schema(WithConfirm({ {"network", {{"type", "string"}}}, {"x1", {{"type", "integer"}}}, {"z1", {{"type", "integer"}}}, {"x2", {{"type", "integer"}}}, {"z2", {{"type", "integer"}}} }),
				json::array({ "network", "x1", "z1", "x2", "z2" })), Permission::Build,
			PlanTool(PlanNetwork) });

		tools.push_back({ "demolish",
			"Demolishes everything in a rectangle of up to 64 x 64 cells (lots, networks, flora), as the game's Demolish command: effect default, none or explode. Previews the lots that would go; plopped buildings (civic, utility, reward, landmark) are refused unless allow_plopped=true. Asks for confirmation; dry_run=true only previews. Cannot be undone.",
			Schema(WithConfirm({ {"x1", {{"type", "integer"}}}, {"z1", {{"type", "integer"}}}, {"x2", {{"type", "integer"}}}, {"z2", {{"type", "integer"}}},
				{"effect", {{"type", "string"}}}, {"allow_plopped", {{"type", "boolean"}}} }), json::array({ "x1", "z1" })), Permission::Build,
			PlanTool(PlanDemolish) });

		tools.push_back({ "place_flora",
			"Places a flora item (tree, shrub) by its exemplar ID at the centre of a cell, on the terrain, through the flora simulator (the game's PlaceFlora command is broken in 1.1.641). Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"flora", {{"type", "string"}}}, {"x", {{"type", "integer"}}}, {"z", {{"type", "integer"}}} }), json::array({ "flora", "x", "z" })), Permission::Build,
			Guarded(PlanFlora, ApplyFlora, [](const json& p) { return Describe(p) + "?"; }) });

		tools.push_back({ "set_terrain",
			"Changes the terrain: altitude (metres) of a cell or a rectangle of up to 64 x 64 cells (SetCellAltitude), or sea_level (SetSeaLevel). Previews the current altitude and the lots in the rectangle; lots are refused unless allow_demolition=true, because changing the terrain under them can destroy them. Asks for confirmation; dry_run=true only previews. Cannot be undone.",
			Schema(WithConfirm({ {"x1", {{"type", "integer"}}}, {"z1", {{"type", "integer"}}}, {"x2", {{"type", "integer"}}}, {"z2", {{"type", "integer"}}},
				{"altitude", {{"type", "number"}}}, {"sea_level", {{"type", "number"}}}, {"allow_demolition", {{"type", "boolean"}}} })), Permission::Build,
			PlanTool(PlanTerrain) });

		tools.push_back({ "set_city_info",
			"Renames the city and/or the mayor (SetCityName, SetMayorName). Asks for confirmation; dry_run=true only previews.",
			Schema(WithConfirm({ {"city_name", {{"type", "string"}}}, {"mayor_name", {{"type", "string"}}} })), Permission::Control,
			PlanTool(PlanCityInfo) });

		tools.push_back({ "load_city",
			"From the region view, opens a city of the loaded region (LoadCity): city is its name (empty = the first city found), size small/medium/large/any (default any), established true (default) or false. Asks for confirmation; dry_run=true only previews. The other city tools answer once the city has loaded.",
			Schema(WithConfirm({ {"city", {{"type", "string"}}}, {"size", {{"type", "string"}}}, {"established", {{"type", "boolean"}}} })), Permission::Control,
			PlanTool(PlanLoadCity, false) });
	}
}
