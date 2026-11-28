# SC4MCP

**A Model Context Protocol server that runs inside SimCity 4.**

[![Build](https://github.com/UlisseWolf/SC4MCP/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/UlisseWolf/SC4MCP/actions/workflows/build.yml)
[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL_v2.1-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%20(x86)-lightgrey)
![Game](https://img.shields.io/badge/SimCity%204-1.1.641-green)
![MCP](https://img.shields.io/badge/MCP-Streamable%20HTTP-purple)

SC4MCP is a DLL plugin for SimCity 4 Deluxe / Rush Hour (version 1.1.641). It starts a
[Model Context Protocol](https://modelcontextprotocol.io) (MCP) server inside the game, so an AI assistant such as
Claude can read the open city (status, budget, buildings, plugin logs) and drive the simulation (pause, speed, let
months pass, run cheats).

It was written to test the balance of [SC4SIM](https://github.com/UlisseWolf/SC4-System-Integration-Module): the
assistant reads the budget building by building, lets a few months pass and compares the numbers with the expected
formulas. SC4MCP does not depend on SC4SIM and works with any plugin set.

---

## Contents

- [Features](#features)
- [Tools](#tools)
- [Requirements](#requirements)
- [Installation](#installation)
- [Connecting an MCP client](#connecting-an-mcp-client) (Claude, Cursor, VS Code, Windsurf, Gemini CLI, Codex, LM Studio)
- [Game commands](#game-commands)
- [Configuration](#configuration)
- [Example session](#example-session)
- [How it works](#how-it-works)
- [Security](#security)
- [Building from source](#building-from-source)
- [Releases](#releases)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [Changelog](Changelog.md)
- [Credits](#credits)
- [License](#license)

## Features

- **Runs in the game process.** No external program, no memory scanning: the DLL uses the game's own COM interfaces
  through the [gzcom-dll](https://github.com/SC4SDK/gzcom-dll) SDK.
- **Read the whole city.** Status, budget down to each building, RCI demand with caps, power, water and garbage,
  education, health, police, jails and fire, ordinances, advisor messages, any city cell, statistics of every map,
  lots and the monthly history of every graph of the Graphs window.
- **Drive the simulation.** Pause, change the speed, run for N game months with live progress, save the city, run
  cheat codes (including the cheats registered by other DLL plugins).
- **Manage the city.** Department and building funding, tax rates, ordinances and zoning.
- **Game commands.** Runs the game's own command system (the commands of Lua test scripts and of the
  `-NetCommandGenerator` socket): place lots and networks, demolish, shape the terrain, move the camera, take
  screenshots, read altitudes and game folders, and any command registered by another DLL plugin, each class behind
  its own `SC4MCP.ini` permission.
- **Plan the city.** Where to build the next school, hospital, fire or police station, what a tax change would
  earn before making it, the cities of the region, CSV exports for spreadsheets.
- **Help modders.** Read any exemplar with its inherited cohort properties, audit the cost and capacity of civic
  buildings, list the plugins and DLLs, find which plugin files override each other, list every cheat, and run test
  scenarios that end in PASS or FAIL.
- **Read plugin logs.** The last lines of any `.log` file in the Plugins folder, for example `SC4SIM.log`.
- **Safe by default.** Listens on `127.0.0.1` only, rejects non-local browser origins, optional bearer token, and
  every group of tools that changes the city can be turned off in `SC4MCP.ini`.
- **Standard MCP.** Streamable HTTP transport (protocol versions `2025-06-18`, `2025-03-26`, `2024-11-05`) with
  tools, resources, prompts and progress notifications.

Building (lots, networks, demolition, terrain) goes through the game's own commands and is off with `AllowBuild=0`;
every such change asks for a confirmation first.

## Tools

### Read the city

| Tool | Parameters | Description |
|---|---|---|
| `get_city_status` | - | Name, size, date, pause state and speed, population, funds, monthly income and expense, RCI demand of each type. |
| `get_budget` | `per_building` (bool) | Funds, totals and loans, every department with its funding and line items; with `per_building=true`, every building with its cells and budget items. |
| `get_demand` | - | Every RCI type: demand, supply, active demand and its range, new demand and supply, economy and tax modifiers; neutral tax rate; the cap fields of the advisor data. |
| `get_taxes` | - | The twelve tax rates. |
| `get_utilities` | `per_plant` (bool, default true) | Power (production, consumption, imports, plants, and per plant: output, load, efficiency, condition, cost), garbage (produced, landfilled, recycled, incinerated, capacity) and the water fields of the advisor data. |
| `get_services` | `per_building` (bool) | Education (coverage, EQ, grades), health (coverage, patients against capacity, life expectancy), police, jails, crime and fire; with `per_building=true`, schools and hospitals with capacity and radius, police and fire stations and jails. |
| `list_buildings` | `name_contains`, `occupant_group` (e.g. `0x1503`), `limit` (1-2000), `offset` | Buildings with exemplar ID, city cells and monthly cost; paged with `next_offset`. |
| `list_lots` | `zone`, `state` (`occupied`, `vacant`, `abandoned`...), `powered` (bool), `limit`, `offset` | Lots with a building: zone, state, occupancy, population and capacity per type, wealth, land value, power and road access. |
| `query_cell` | `x`, `z` | One cell: zone, lot and building, land value, pollution, crime, police and fire coverage, power, congestion. |
| `get_map_stats` | `map`, `top` (0-50), `developed_only` (bool) | Min, max, average, histogram and worst cells of `zones`, `land_value`, `crime`, `air_pollution`, `water_pollution`, `garbage`, `fire_protection`, `police_coverage`, `congestion` or `powered`. |
| `list_graphs` | - | Every graph of the Graphs window with the history type and label of each line. |
| `get_history` | `graph` or `history_types`, `months` (1-600) | Monthly values of a graph's lines, or of any history types. |
| `list_ordinances` | - | Every ordinance with its state and monthly income or cost. |
| `get_advisors` | `max_messages` (1-50) | The advisors with their current messages, and the popup message. |
| `get_game_data` | `contains` (word or list), `limit` | The raw advisor data (`game` Lua table: coverage, grades, capacities, rates...). |
| `get_transport` | - | Commuting, transit (bus, rail, subway, monorail, ferry, airport, seaport), station use, congestion and potholes, and statistics of the congestion map. |
| `get_loans` | - | Every loan with payment and remaining time, total borrowed, what the city can borrow now, loan step and cost. |
| `get_neighbor_deals` | - | Deals with the neighbouring cities (power, water, garbage): city, amount, use, monthly cost; total income and expense; adjacent cities. |
| `render_map` | `map`, `scale` (1-8), `fixed_range` (bool), `highlight` | Draws a map as a PNG image the assistant can look at: `zones`, `land_value`, `crime`, `air_pollution`, `water_pollution`, `garbage`, `fire_protection`, `police_coverage`, `congestion`, `powered`. Green is good, red bad, water blue. `highlight` paints in magenta the `abandoned` lots, the `unpowered` lots, the lots with `no_road`, the buildings of an occupant group (`0x1503`) or the buildings whose name contains a text (`name:clinic`). |
| `get_region` | - | Every city of the region with position and size and, for established cities, population, jobs and mayor rating; region totals. |
| `export_csv` | `what` (`buildings`, `lots`, `budget`, `history`, `map`), `arguments` | Writes a CSV file (UTF-8, opens in Excel) to `SC4MCP_exports` next to the DLL; `arguments` go to the matching tool, e.g. `{"graph": "0x06", "months": 24}` or `{"map": "land_value"}`. |
| `list_changes` | - | The changes made by the tools in this session, newest first, and whether each can be undone. |
| `read_log` | `file` (default `SC4SIM.log`), `lines` (1-1000) | The last lines of a `.log` file next to the DLL. |

### Plan, diagnose and compare

| Tool | Parameters | Description |
|---|---|---|
| `diagnose_growth` | - | Why the city does or does not grow: demand per type, undeveloped zones, abandoned lots by the reason the game recorded (low desirability, no power, no water, commute time, low demand), unpowered lots, lots without road access, development failures, power margin, taxes above the neutral rate; with plain findings. |
| `diagnose_cell` | `x`, `z` | Why one cell does not grow or was abandoned, with plain reasons. |
| `suggest_location` | `service` (`school`, `hospital`, `fire`, `police`), `count` (1-10), `radius_m` | The areas with the most people outside the coverage of the existing buildings of that kind (residents for schools and hospitals, residents and workers for fire and police), with a free cell nearby and the coverage now. The radius defaults to the median of the existing buildings. |
| `tax_what_if` | `type` + `rate`, or `rates` (object) | Estimated monthly tax income with other rates, without changing anything: income per type from the assessed value, calibrated on the current income, with the distance from the neutral rate. |
| `get_building_properties` | `x` + `z`, or `exemplar`, or `type` + `group` + `instance` | The properties of a building (or of any exemplar or cohort) as the game loaded them, after every plugin override: its in-city values and its exemplar. Property names come from `new_properties.xml` when it is next to the DLL or in `%APPDATA%\sc4pimx`. |
| `snapshot_save` | `name` (default `default`) | Saves a copy of status, budget, demand, taxes, services and utilities to `SC4MCP_snapshots`, so it survives a restart of the game. |
| `snapshot_compare` | `name`, `min_percent`, `limit` | Only the values that changed since the snapshot (before, after, change, percent), largest first. |
| `snapshot_list`, `snapshot_delete` | `name` | Manage the snapshots (at most 30). |

### Modding tools

| Tool | Parameters | Description |
|---|---|---|
| `read_resource` | `type`, `group`, `instance` | A resource as the game loaded it: exemplars and cohorts with their own properties, the chain of parent cohorts and the properties inherited from them; LTEXT as text; for other types, whether it exists. |
| `audit_buildings` | `family` (`school`, `hospital`, `fire`, `police`, `power`) | One row per civic or utility exemplar in the city: capacity, monthly cost, plop cost, radius, cost per unit. Flags the buildings far from the median of their family and those with BSC-style plop costs (less than 3 months of upkeep). |
| `list_plugins` | `limit` | The Plugins folder: files and bytes by extension, plugin files per top-level folder, every DLL with version, log and ini. |
| `find_plugin_conflicts` | `type`, `group`, `file_contains`, `limit`, `refresh` | Reads the index of every plugin file and lists the resources defined by more than one file, grouped by file pair: which file wins, which one it overrides, how many resources, an example. The index is kept for 10 minutes. |
| `find_resource_file` | `type`, `group`, `instance`, `refresh` | Which plugin files define a resource, in load order, and which one the game uses. |
| `list_cheats` | - | Every cheat code registered now, by the game and by the DLL plugins. |

The load order used by `find_plugin_conflicts` and `find_resource_file` is an estimate (the files of a folder before
its subfolders, each level in alphabetical order, the later file wins). Many overrides are intended: patches and mods
placed in late folders on purpose.

### Control the simulation and the city

| Tool | Parameters | Description | Requires |
|---|---|---|---|
| `set_speed` | `speed` (0-4) | Pause (0) or run at speed 1 to 4; returns the state once the game has applied it. | `AllowControl=1` |
| `wait_months` | `months` (1-120), `speed` (1-4), `timeout_seconds` (5-600, default 50) | Runs N game months, then pauses and returns the city status. Sends progress notifications. | `AllowControl=1` |
| `save_city` | - | Saves the city, as Ctrl+S. | `AllowControl=1` |
| `set_funding` | `department`, `percent` | Funding slider of a budget department. | `AllowControl=1` |
| `set_building_funding` | `x`, `z`, `percent`, `purpose` (optional) | Local funding of one building. | `AllowControl=1` |
| `set_tax` | `type` (`r_low`... `i_hightech`, `residential`, `commercial`, `industrial`, `all`), `rate` (0-20) | Tax rate, in 0.1 steps. | `AllowControl=1` |
| `set_ordinance` | `ordinance` (ID or part of the name), `on` (bool) | Turns an ordinance on or off. | `AllowControl=1` |
| `take_loan` | `amount` | Takes a loan (rounded to the loan step, within the borrowing limit). Cannot be undone. | `AllowControl=1` |
| `undo_last_change` | - | Restores the values before the last funding, tax or ordinance change. | `AllowControl=1` |
| `place_zone` | `zone`, `x1`, `z1`, `x2`, `z2`, `allow_demolition` (bool) | Zones or dezones (`zone=none`) a rectangle of up to 64 x 64 cells. Developed lots are refused unless `allow_demolition=true` (changing their zone demolishes them); plopped buildings are never touched. | `AllowBuild=1` |
| `run_cheat` | `cheat` | Runs a cheat code as if typed in the Ctrl+X box. | `AllowCheats=1` |
| `run_scenario` | `steps`, `stop_on_failure`, `dry_run`, `confirm` | Runs a list of steps (a tool with its arguments, `wait_months`, and checks on the results) and returns PASS or FAIL with every step; progress per step; one confirmation for the whole scenario. See [Test scenarios](#test-scenarios). | `AllowControl=1` |

The tools that change the city (`set_funding`, `set_building_funding`, `set_tax`, `set_ordinance`, `take_loan`,
`undo_last_change`, `place_zone`, `run_cheat`) accept `dry_run=true` (only describe the change) and, with `ConfirmChanges=1` (default), ask before
applying: a client that supports MCP elicitation shows the user a confirmation; with other clients the tool returns a
preview with `confirmation_required` and applies the change only when called again with `confirm=true`.

### Game commands

SimCity 4 has a command system for Lua test scripts, the `-NetCommandGenerator` socket and the cheat box, documented in
[sc4-game-command-documentation](https://github.com/0xC0000054/sc4-game-command-documentation). SC4MCP runs these
commands directly through the game's command server: no command line argument and no socket are needed.

| Tool | Parameters | Description | Requires |
|---|---|---|---|
| `list_game_commands` | `contains`, `class`, `include_unregistered` (bool) | The commands registered now, including those of other DLL plugins, with parameter info, class, summary and whether the settings allow them. | - |
| `query_game_command` | `command` | Runs a read or view command (`GetCellAltitude 10 20`, `GetDirectory UserPlugin`, `Zoom3`...) and returns its output and parsed values. | - |
| `get_game_info` | - | View (region or city), real date and time, frame rate, memory, the game folders, camera and, in a city, name, mayor, size, sea level and simulation date. Works in the region view. | - |
| `get_terrain` | `x1`, `z1`, `x2`, `z2`, `step` | Altitude of a cell or rectangle: min, max, average, cells under the sea and the grid (up to 1024 samples). | - |
| `set_camera` | `x`, `z`, `zoom` (0-5), `rotate` (-3..3), `lighting` (`day`, `night`, `cycle`), `grid`, `signs_and_labels`, `terrain_contours`, `ui` | Moves the camera, zooms, rotates, sets the lighting and toggles overlays; returns the camera position. | - |
| `take_snapshot` | `format` (`png`, `jpg`, `bmp`), `crop`, `return_image` | Screenshot of the game window to `SC4MCP_screenshots`, returned as an image the assistant can look at. | - |
| `place_lot` | `lot`, `x`, `z`, `facing`, `plop_effects` | Places a lot by its lot configuration instance ID; previews name, size and covered cells. | `AllowBuild=1` |
| `place_network` | `network`, `x1`, `z1`, `x2`, `z2` | Draws a road, rail, highway, street, pipe, power line, avenue, subway, light rail, monorail, one-way road, dirt road or ground highway. | `AllowBuild=1` |
| `demolish` | `x1`, `z1`, `x2`, `z2`, `effect`, `allow_plopped` | Demolishes a rectangle of up to 64 x 64 cells; plopped buildings only with `allow_plopped=true`. | `AllowBuild=1` |
| `place_flora` | `flora`, `x`, `z` | Places a tree or shrub at the centre of a cell. | `AllowBuild=1` |
| `set_terrain` | `x1`, `z1`, `x2`, `z2` + `altitude`, or `sea_level`; `allow_demolition` | Sets the altitude of up to 64 x 64 cells, or the sea level; cells with lots only with `allow_demolition=true`. | `AllowBuild=1` |
| `set_city_info` | `city_name`, `mayor_name` | Renames the city and the mayor. | `AllowControl=1` |
| `load_city` | `city`, `size`, `established` | From the region view, opens a city. | `AllowControl=1` |
| `run_game_command` | `command` | Runs any command line, after its class is checked (see below). | `AllowControl=1` |

Every command has a class, which decides what it needs:

| Class | Examples | Needs |
|---|---|---|
| `read` | `GetAppState`, `GetCellAltitude`, `GetSimulationDate`, `GetDirectory`, `GetMemInfo` | nothing |
| `view` | `SetViewTarget`, `Zoom0`-`Zoom5`, `RotateCW`, `ToggleGridVisibility`, the `...Tool` selectors | nothing |
| `control` | `SimSpeedFast`, `GamePause`, `SaveCity`, `SetCityName`, `LoadCity` | `AllowControl=1` and a confirmation |
| `modal` | `GZWinMessageBox`, `Open...Dialog` (the game waits for the player) | `AllowControl=1` and a confirmation |
| `build` | `PlaceLot`, `PlaceNetwork`, `PlaceZone`, `Demolish`, `SetCellAltitude`, `SetSeaLevel`, `ToxicSpill` | `AllowBuild=1` and a confirmation |
| `unknown` | commands registered by other DLL plugins | `AllowCheats=1` and a confirmation |
| `dangerous` | `QuitGame`, `QuitCity`, `ExecuteScript`, `ViewWebBrowser`, `TakeSnapshot` at any path, `GZWin...` window commands | `AllowDangerousCommands=1` and a confirmation |
| `forbidden` | `CreateException` | never run |

City commands are registered only while a city is open and region commands only in the region view;
`list_game_commands` shows what is available now. Changes made by game commands are listed by `list_changes` and
cannot be undone.

### Test scenarios

`run_scenario` turns a balance test into a repeatable script. Each step can call a tool, wait some months and check a
value with a dotted path (`status.funds`, `departments.0.funding_percent`); without a `tool`, a check reads the result
of its own step.

```json
{
  "steps": [
    { "tool": "snapshot_save", "arguments": { "name": "before" } },
    { "tool": "set_tax", "arguments": { "type": "residential", "rate": 9 } },
    { "wait_months": 12, "speed": 4 },
    { "check": { "tool": "get_city_status", "path": "funds", "op": ">", "value": 0 } },
    { "check": { "tool": "get_demand", "path": "types.R$.demand", "op": ">", "value": 0 } }
  ]
}
```

Operators: `>`, `>=`, `<`, `<=`, `==`, `!=`, `contains`, `exists`. Steps that change the city are confirmed once,
before the scenario starts; the result lists every step with the expected and actual value of its check.

Cells are `(x, z)` city cells, as `list_buildings`, `list_lots` and `query_cell` report them. Common occupant groups for
`list_buildings`: `0x1400` power, `0x1503` schools, `0x1507` health.

### Resources and prompts

| Resource | Content |
|---|---|
| `sc4://city/status`, `sc4://city/budget`, `sc4://city/demand` | Same as `get_city_status`, `get_budget`, `get_demand`. |
| `sc4://city/utilities`, `sc4://city/services`, `sc4://city/ordinances` | Same as `get_utilities`, `get_services`, `list_ordinances`. |
| `sc4://city/transport`, `sc4://city/loans`, `sc4://city/neighbors`, `sc4://city/growth` | Same as `get_transport`, `get_loans`, `get_neighbor_deals`, `diagnose_growth`. |
| `sc4://log/SC4SIM.log`, `sc4://log/SC4MCP.log` | The last 200 lines of the log. |

Resources can be subscribed to: the server then sends `notifications/resources/updated` for them at every new game
month (see [Notifications](#notifications)).

### Notifications

A client that opens the notification stream (`GET /mcp` with `Accept: text/event-stream`, done automatically by
clients that support it) receives, while the game runs:

- `notifications/message` (MCP logging) at every new game month, with date, population, funds and monthly balance;
- warnings when the funds go below zero, the month ends in deficit, the power plants run at full load or buildings
  lose power, and notices when a city is opened or closed;
- `notifications/resources/updated` for the subscribed resources at every new month.

`logging/setLevel` chooses the lowest level sent (`info` by default; `warning` keeps only the warnings).

| Prompt | Arguments | What it asks the assistant |
|---|---|---|
| `balance_test` | `months` (default 12), `focus` | Baseline, run N months, compare budget, services, utilities and demand, report what looks unbalanced. |
| `city_report` | - | A short report on finances, demand, services, utilities and the main problems. |
| `find_problems` | - | The city's weak points (deficits, shortages, low coverage, abandoned buildings, crime and pollution hot spots) with fixes. |

## Requirements

- SimCity 4 Deluxe or Rush Hour, **version 1.1.641** (Windows; the Steam and GOG versions are 641).
- Windows 10 or 11.
- An MCP client running on the same PC that supports the Streamable HTTP transport (Claude Code, Cursor, VS Code,
  Windsurf, Gemini CLI, Codex, LM Studio...), or Node.js for the
  [`mcp-remote`](https://www.npmjs.com/package/mcp-remote) bridge (Claude Desktop and stdio-only clients).

## Installation

1. Download `SC4MCP.dll` and `SC4MCP.ini` from the [releases](../../releases), or [build them](#building-from-source).
2. Copy both files to the **top level** of the Plugins folder (`Documents\SimCity 4\Plugins`), not to a subfolder:
   the game loads DLL plugins only from there.
3. Start the game. `SC4MCP.log` appears next to the DLL and should contain:
   ```
   MCP server listening on http://127.0.0.1:8765/mcp
   ```
4. Open a city. The tools answer only while a city is open.

Windows may ask whether SimCity 4 may use the network. The server listens only on `127.0.0.1`, so the firewall
prompt can be denied without breaking it.

To check that the server is running, open `http://127.0.0.1:8765/health` in a browser.

## Connecting an MCP client

The endpoint is `http://127.0.0.1:8765/mcp` (Streamable HTTP). The client must run on the same PC as the game.

### Claude Code

```sh
claude mcp add --transport http sc4 http://127.0.0.1:8765/mcp
```

### Claude Desktop

The server is on your PC, so it must be added as a local server: a custom connector added in
*Settings > Connectors* runs from the cloud and cannot reach `127.0.0.1`. With Node.js installed, open
*Settings > Developer > Edit Config* and add to `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "sc4": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "http://127.0.0.1:8765/mcp"]
    }
  }
}
```

Quit Claude Desktop completely (including the tray icon) and start it again.

### Other AI clients

SC4MCP is a standard MCP server, so any MCP client that runs **on the same PC as the game** can use it. Most clients
take the URL directly; the configuration key differs from client to client.

| Client | Configuration file | Key for the URL |
|---|---|---|
| Claude Code | `claude mcp add` (see above) | - |
| Claude Desktop | `claude_desktop_config.json` | via `mcp-remote` (see above) |
| Cursor | `%USERPROFILE%\.cursor\mcp.json` or `.cursor/mcp.json` | `url` |
| VS Code (GitHub Copilot agent mode) | `.vscode/mcp.json` or the user `mcp.json` | `type: "http"` + `url` |
| Windsurf | `%USERPROFILE%\.codeium\windsurf\mcp_config.json` | `serverUrl` |
| Gemini CLI | `%USERPROFILE%\.gemini\settings.json` | `httpUrl` |
| Codex CLI / ChatGPT desktop app | `%USERPROFILE%\.codex\config.toml` | `url` |
| LM Studio (local models) | `mcp.json` (Program > Install > Edit mcp.json) | `url` |

> SC4MCP has been tested with Claude Code and Claude Desktop. The other configurations follow each client's
> documentation; please open an issue if one of them needs a change.

#### Cursor

`~/.cursor/mcp.json` (all projects) or `.cursor/mcp.json` (one project):

```json
{
  "mcpServers": {
    "sc4": {
      "url": "http://127.0.0.1:8765/mcp"
    }
  }
}
```

#### VS Code (GitHub Copilot agent mode)

`.vscode/mcp.json` in the workspace, or *MCP: Open User Configuration* from the Command Palette. VS Code uses the key
`servers` and needs `"type": "http"`, otherwise it tries to start a local process:

```json
{
  "servers": {
    "sc4": {
      "type": "http",
      "url": "http://127.0.0.1:8765/mcp"
    }
  }
}
```

Or from a terminal:

```sh
code --add-mcp "{\"name\":\"sc4\",\"type\":\"http\",\"url\":\"http://127.0.0.1:8765/mcp\"}"
```

The tools are available in Copilot Chat in **Agent** mode.

#### Windsurf

`%USERPROFILE%\.codeium\windsurf\mcp_config.json` (or *Windsurf Settings > Cascade > MCP Servers > View raw
config*). Windsurf uses `serverUrl`, not `url`:

```json
{
  "mcpServers": {
    "sc4": {
      "serverUrl": "http://127.0.0.1:8765/mcp"
    }
  }
}
```

Press the refresh button of the MCP panel after saving.

#### Gemini CLI

`~/.gemini/settings.json` (all projects) or `.gemini/settings.json` (one project). `httpUrl` selects the Streamable
HTTP transport (`url` would mean SSE):

```json
{
  "mcpServers": {
    "sc4": {
      "httpUrl": "http://127.0.0.1:8765/mcp"
    }
  }
}
```

Run `/mcp` inside Gemini CLI to check that the seven tools are listed.

#### Codex CLI and the ChatGPT desktop app

The Codex CLI, its IDE extension and the ChatGPT desktop app share `~/.codex/config.toml`:

```sh
codex mcp add sc4 --url http://127.0.0.1:8765/mcp
```

or by hand:

```toml
[mcp_servers.sc4]
url = "http://127.0.0.1:8765/mcp"
tool_timeout_sec = 300   # wait_months can take longer than the default 60 seconds
```

Run `/mcp` inside Codex to check the connection.

#### LM Studio (local models)

LM Studio 0.3.17 or later can give the SC4MCP tools to a model running on your own PC, with no cloud service. Open
*Program > Install > Edit mcp.json* (Cursor notation):

```json
{
  "mcpServers": {
    "sc4": {
      "url": "http://127.0.0.1:8765/mcp"
    }
  }
}
```

Use a model trained for tool calling: small models often call the tools with wrong parameters or stop after one call.

#### Other clients

Any other client that speaks MCP over Streamable HTTP can connect to `http://127.0.0.1:8765/mcp`. A client that only
supports stdio can use the `mcp-remote` bridge, exactly as in the Claude Desktop example.

### Assistants in the cloud (ChatGPT web, claude.ai)

Web assistants call MCP servers from their own servers on the internet, so they cannot reach `127.0.0.1`: ChatGPT's
developer mode and the custom connectors of claude.ai need a public HTTPS address. A tunnel (for example OpenAI's
Secure MCP Tunnel, ngrok or Cloudflare Tunnel) would make SC4MCP reachable, but **without `AuthToken` SC4MCP has no
authentication**: anyone who knows the tunnel address could pause your game, change your taxes or run cheats. If you
do it anyway, set a long random `AuthToken`, consider `AllowControl=0`, `AllowCheats=0` and `AllowBuild=0`, and close
the tunnel when you are done. Prefer a desktop client from the list above.

### Long calls

`wait_months` holds the call open until the months have passed (by default at most 50 seconds, up to
`timeout_seconds`). When the client sends a `progressToken` and accepts `text/event-stream`, SC4MCP sends a progress
notification at every new game month and the result at the end, on the same response; clients that reset their
timeout on progress then never cut the call. Other clients may stop waiting earlier: raise the client's tool timeout
(for example `tool_timeout_sec` in Codex) before asking for more than 50 seconds.

## Configuration

`SC4MCP.ini` sits next to the DLL. Restart the game after a change.

| Key | Default | Description |
|---|---|---|
| `Enabled` | `1` | `0` turns the plugin off. |
| `Port` | `8765` | TCP port on `127.0.0.1`. Change it if another program uses 8765. |
| `AllowControl` | `1` | `0` removes `set_speed`, `wait_months`, `save_city`, `set_funding`, `set_building_funding`, `set_tax`, `set_ordinance`, `set_city_info`, `load_city` and `run_game_command`. |
| `AllowCheats` | `1` | `0` removes `run_cheat` and refuses the commands of other DLL plugins in `run_game_command`. |
| `AllowBuild` | `1` | `0` removes `place_zone`, `place_lot`, `place_network`, `demolish`, `place_flora` and `set_terrain`, and refuses the build commands of `run_game_command`. |
| `AllowDangerousCommands` | `0` | `1` lets `run_game_command` run the dangerous game commands (quit, scripts, browser, files at any path, window commands). `CreateException` is never run. |
| `ReadOnly` | `0` | `1` turns off `AllowControl`, `AllowCheats`, `AllowBuild` and `AllowDangerousCommands` at once. |
| `ConfirmChanges` | `1` | Tools that change the city ask for a confirmation (see [Tools](#tools)); `0` applies at once. |
| `AuthToken` | (empty) | When set, every request to `/mcp` needs `Authorization: Bearer <AuthToken>`. `auto` generates a random token at the next start and writes it to the file. |

**Client configurations.** With the game running, open `http://127.0.0.1:8765/setup` in a browser: the page lists
the configuration of every client in this README (Claude Code, Claude Desktop, Cursor, VS Code, Windsurf, Gemini CLI,
Codex, LM Studio) with the right port and the token already filled in, ready to copy.

To use a token by hand, add it to the client configuration as a header. For example in Cursor:

```json
{ "mcpServers": { "sc4": { "url": "http://127.0.0.1:8765/mcp", "headers": { "Authorization": "Bearer my-secret" } } } }
```

and with `mcp-remote`: `"args": ["-y", "mcp-remote", "http://127.0.0.1:8765/mcp", "--header", "Authorization: Bearer my-secret"]`.

## Example session

A typical balance test, as an assistant would run it:

```text
> get_city_status
  Testville, March 2005, paused - population 48,210, funds §412,300, income §18,940/month, expense §17,120/month

> list_buildings  occupant_group=0x1503
  12 schools: Elementary School (0x...), 4 cells, §310/month ...

> wait_months  months=6 speed=3
  ran 6 months - September 2005, population 51,880, funds §421,700

> get_budget  per_building=true
  Education 100% - 12 schools, §3,720/month ...

> read_log  file=SC4SIM.log lines=20
```

## How it works

```
 MCP client ──HTTP──▶ HttpServer (worker thread, 127.0.0.1)
                          │  JSON-RPC request
                          ▼
                    MainThreadQueue ──▶ TickService (game thread, every frame, also when paused)
                                              │
                                              ▼
                                         Tools ──▶ game COM interfaces (city, budget, simulator, cheats)
```

- **Game objects are touched only on the game thread.** The HTTP thread posts each request to a queue that a
  framework tick service empties once per frame, also while the game is paused. If the game is minimized, loading or
  showing a modal dialog, a request times out after 10 seconds.
- **One request at a time.** While `wait_months` runs (up to `timeout_seconds`), other calls wait.
- **Monthly values are the game's own estimates.** The department totals of the budget window update at the start
  of each game month.
- **Education, health and water come from the advisor data.** These simulators have no interface in gzcom-dll, so
  SC4MCP reads the `game` Lua table that the game's advisors and UI read; it is filled once a month, so a city opened
  a moment ago may not have it yet.
- **Tax groups are verified before use.** The budget simulator's tax group IDs are not documented: `set_tax` tries
  the possible numberings and uses the one whose rates match the advisor data, so a wrong guess never writes a rate.

| Source file | Role |
|---|---|
| `src/McpDllDirector.cpp` | DLL entry point (COM director): reads the INI, starts the server and the tick service. |
| `src/HttpServer.cpp` | Minimal HTTP/1.1 server, one thread per connection (at most 16), origin and host checks. |
| `src/McpServer.cpp` | JSON-RPC 2.0 / MCP: `initialize`, tools, resources, prompts, progress and elicitation over server-sent events, token check, `/health` and `/setup`. |
| `src/MainThreadQueue.cpp`, `src/TickService.cpp` | Hand-off of requests to the game thread. |
| `src/Tools.cpp` | Tool registry, core tools (status, budget, speed, `wait_months`, cheats), resources and prompts. |
| `src/ToolsRead.cpp` | Read-only tools (demand, utilities, services, cells, maps, lots, history, ordinances, advisors). |
| `src/ToolsControl.cpp` | Funding, taxes, ordinances, saving and zoning, with previews for the confirmations. |
| `src/ToolsDiagnose.cpp` | Growth diagnostics, building properties and snapshots. |
| `src/ToolsExtra.cpp` | Transport, loans, neighbour deals, map images, change history and undo. |
| `src/ToolsMore.cpp` | Service locations, CSV export, region, building audit, resources, plugins and overrides, cheats, scenarios. |
| `src/ToolsCommands.cpp` | Game commands through the game's command server: classes and permissions, the generic tools and the curated build, terrain, camera and snapshot tools. |
| `src/ToolsCommon.cpp` | Helpers shared by the tool modules (game thread, buildings, advisor data). |
| `src/McpLog.cpp` | `SC4MCP.log`. |

## Security

- The server binds to `127.0.0.1` only and is never reachable from other machines.
- Requests whose `Origin` header is not a local address, or whose `Host` header is not a loopback name, are refused
  (protection against web pages and DNS rebinding).
- Changes to the city ask for a confirmation by default (`ConfirmChanges=1`), and `ReadOnly=1` turns them all off.
- By default there is no authentication: any program running on the same PC can call the server while the game is
  open. Set `AuthToken` to require a bearer token, set `AllowControl=0`, `AllowCheats=0` and `AllowBuild=0` for a
  read-only server, or `Enabled=0` when you do not need it.
- Every change to the city (speed, funding, taxes, ordinances, zoning, saving, cheats, game commands) and every
  confirmation is written to `SC4MCP.log`.
- Game commands are classified before they run; the dangerous ones (quitting, running scripts, writing files at any
  path, window commands that can block input) need `AllowDangerousCommands=1`, and `CreateException` is never run.
- `read_log` reads only `.log` files in the Plugins folder. The plugin tools only read the Plugins folder; files are
  written only to `SC4MCP_snapshots`, `SC4MCP_exports` and `SC4MCP_screenshots` next to the DLL.

## Building from source

Visual Studio 2022 with the *Desktop development with C++* workload. The game is 32-bit, so the DLL must be built
for **x86 (Win32)**.

**Get the source**

The [gzcom-dll](https://github.com/SC4SDK/gzcom-dll) SDK is a git submodule in `vendor/gzcom-dll`, so clone with
submodules:

```sh
git clone --recursive -b staging https://github.com/UlisseWolf/SC4MCP.git
```

In a copy cloned without `--recursive`, or to move the SDK to its latest commit, run:

```sh
git submodule update --init --remote
```

A ZIP downloaded from GitHub does not contain the submodule: clone the repository instead.

**Visual Studio**

1. Open `SC4MCP.sln`.
2. Select **Release | x86** and build.
3. The DLL is `bin\Release\SC4MCP.dll`.

**CMake**

```sh
cmake -S . -B build -A Win32
cmake --build build --config Release
```

The other dependency, [nlohmann/json](https://github.com/nlohmann/json), is included in `vendor/nlohmann`; nothing
else needs to be installed.

## Releases

The workflows in `.github/workflows` run only for the `master` branch:

- `build.yml` builds the DLL on every push to `master` and every pull request into `master`, and keeps it as a
  workflow artifact with `SC4MCP.ini`. Work on `staging` is built when it is merged or proposed into `master`.
- `release.yml` runs for a `vMAJOR.MINOR.PATCH` tag (or `v1.4.0-rc.1` for a prerelease). It refuses tags on commits
  that are not in `master` and tags that do not match `kServerVersion` in `src/McpServer.cpp`. Then the reusable
  workflow of [sc4-dll-actions](https://github.com/memo33/sc4-dll-actions) builds a zip with the DLL, the INI,
  README, changelog, licenses and `SHA256SUMS.txt`, attests the zip and the DLL, and leaves them in a draft release.

To release, merge `staging` into `master`, then tag the merge commit and push the tag:

```sh
git checkout master && git merge staging && git push origin master
git tag v1.4.0 && git push origin v1.4.0
```

Anyone can check a download:

```sh
gh attestation verify SC4MCP-v1.4.0-win32.zip --repo UlisseWolf/SC4MCP \
  --deny-self-hosted-runners --signer-repo memo33/sc4-dll-actions \
  --signer-digest c7aa3d2d748a9bd52a573a79f1f6ba78dde6f25f
```

Publishing the draft starts `sc4pac.yml`, which updates the package in an
[sc4pac](https://memo33.github.io/sc4pac/) channel with
[sc4pac-release-action](https://github.com/caspervg/sc4pac-release-action). It skips prereleases and is off until:

1. a fork of the channel exists (default `sebamarynissen/simtropolis-channel`),
2. the environment `sc4pac` (tags `v*` only) has the secret `SC4PAC_FORK_TOKEN`, a fine-grained token with
   *Contents: Read and write* on that fork,
3. the repository variable `SC4PAC_ASSET_ID` names the release zip's asset in the channel (`SC4PAC_CHANNEL` for
   another channel).

Dependabot proposes updates of the actions to `master` once a month.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| No `SC4MCP.log` | The DLL is not in the top level of Plugins, or the game is not 1.1.641. |
| Log says "Cannot listen on 127.0.0.1:8765" | Another program uses 8765: set another `Port` and update the client URL. |
| Tools return "No city is open" | Open a city: the region view has no city to read. |
| Tools time out after 10 s | The game is minimized, loading, or showing a dialog. Bring it to the front. |
| Claude Desktop does not list the tools | Add the server through `mcp-remote` (not as a custom connector), then restart Claude Desktop completely. |
| ChatGPT says "Unsafe URL" | ChatGPT web cannot reach `127.0.0.1`: use a desktop client (see [Assistants in the cloud](#assistants-in-the-cloud-chatgpt-web-claudeai)). |
| VS Code tries to run a command | `"type": "http"` is missing, or the entry is under `mcpServers` instead of `servers`. |
| Windsurf connects to nothing, no error | The key must be `serverUrl`, not `url`. |
| `wait_months` is cut off by the client | Raise the client's tool timeout (see [Long calls](#long-calls)). |
| `http://127.0.0.1:8765/health` does not open | The game is not running, or `Enabled=0`. |

## Contributing

Issues and pull requests are welcome. Please:

- keep every game call on the game thread (through `MainThreadQueue`);
- add new tools to the matching `src/Tools*.cpp` module with a JSON schema and the right permission, and
  document them in this README;
- test with SimCity 4 1.1.641 and describe the test in the pull request.

## Changelog

Every version and its changes are listed in [Changelog.md](Changelog.md).

## Credits

- [gzcom-dll](https://github.com/SC4SDK/gzcom-dll) by the SC4SDK contributors, from the original SDK by
  [nsgomez](https://github.com/nsgomez/gzcom-dll): the interfaces of the game that SC4MCP calls.
- [sc4-game-command-documentation](https://github.com/0xC0000054/sc4-game-command-documentation) by 0xC0000054: the
  game commands, their parameters and the command server interfaces.
- [dbpf-mcp](https://github.com/caspervg/dbpf-mcp) by caspervg: the reference for reading DBPF files, used for
  `find_plugin_conflicts` and `find_resource_file`.
- [nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann: JSON for the MCP protocol.
- [sc4-seamless-region](https://github.com/caspervg/sc4-seamless-region) by caspervg: the GitHub workflows, and
  [sc4-dll-actions](https://github.com/memo33/sc4-dll-actions) by memo33: the attested release build.

## License

SC4MCP is released under the [GNU Lesser General Public License, version 2.1](LICENSE).

Third-party code in `vendor/`:

| Component | License |
|---|---|
| [gzcom-dll](https://github.com/SC4SDK/gzcom-dll) SDK (`vendor/gzcom-dll`, git submodule) | LGPL 2.1, see `vendor/gzcom-dll/LICENSE` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (`vendor/nlohmann`) | MIT, see `vendor/nlohmann/LICENSE.MIT` |

SimCity 4 is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by Electronic Arts.
