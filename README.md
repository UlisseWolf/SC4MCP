# SC4MCP

**A Model Context Protocol server that runs inside SimCity 4.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
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
- [Configuration](#configuration)
- [Example session](#example-session)
- [How it works](#how-it-works)
- [Security](#security)
- [Building from source](#building-from-source)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [Changelog](#changelog)
- [License](#license)

## Features

- **Runs in the game process.** No external program, no memory scanning: the DLL uses the game's own COM interfaces
  through the [gzcom-dll](https://github.com/nsgomez/gzcom-dll) SDK.
- **Read the whole city.** Status, budget down to each building, RCI demand with caps, power, water and garbage,
  education, health, police, jails and fire, ordinances, advisor messages, any city cell, statistics of every map,
  lots and the monthly history of every graph of the Graphs window.
- **Drive the simulation.** Pause, change the speed, run for N game months with live progress, save the city, run
  cheat codes (including the cheats registered by other DLL plugins).
- **Manage the city.** Department and building funding, tax rates, ordinances and zoning.
- **Read plugin logs.** The last lines of any `.log` file in the Plugins folder, for example `SC4SIM.log`.
- **Safe by default.** Listens on `127.0.0.1` only, rejects non-local browser origins, optional bearer token, and
  every group of tools that changes the city can be turned off in `SC4MCP.ini`.
- **Standard MCP.** Streamable HTTP transport (protocol versions `2025-06-18`, `2025-03-26`, `2024-11-05`) with
  tools, resources, prompts and progress notifications.

SC4MCP does not place buildings or networks (roads, rails, pipes, power lines): those stay with the player.

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
| `render_map` | `map`, `scale` (1-8), `fixed_range` (bool) | Draws a map as a PNG image the assistant can look at: `zones`, `land_value`, `crime`, `air_pollution`, `water_pollution`, `garbage`, `fire_protection`, `police_coverage`, `congestion`, `powered`. Green is good, red bad, water blue. |
| `list_changes` | - | The changes made by the tools in this session, newest first, and whether each can be undone. |
| `read_log` | `file` (default `SC4SIM.log`), `lines` (1-1000) | The last lines of a `.log` file next to the DLL. |

### Diagnose and compare

| Tool | Parameters | Description |
|---|---|---|
| `diagnose_growth` | - | Why the city does or does not grow: demand per type, undeveloped zones, abandoned lots by the reason the game recorded (low desirability, no power, no water, commute time, low demand), unpowered lots, lots without road access, development failures, power margin, taxes above the neutral rate; with plain findings. |
| `diagnose_cell` | `x`, `z` | Why one cell does not grow or was abandoned, with plain reasons. |
| `get_building_properties` | `x` + `z`, or `exemplar`, or `type` + `group` + `instance` | The properties of a building as the game loaded them, after every plugin override: its in-city values and its exemplar. |
| `snapshot_save` | `name` (default `default`) | Keeps a copy of status, budget, demand, taxes, services and utilities in memory. |
| `snapshot_compare` | `name`, `min_percent`, `limit` | Only the values that changed since the snapshot (before, after, change, percent), largest first. |
| `snapshot_list`, `snapshot_delete` | `name` | Manage the snapshots (at most 10, lost when the game closes). |

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

The tools that change the city (`set_funding`, `set_building_funding`, `set_tax`, `set_ordinance`, `take_loan`,
`undo_last_change`, `place_zone`, `run_cheat`) accept `dry_run=true` (only describe the change) and, with `ConfirmChanges=1` (default), ask before
applying: a client that supports MCP elicitation shows the user a confirmation; with other clients the tool returns a
preview with `confirmation_required` and applies the change only when called again with `confirm=true`.

Cells are `(x, z)` city cells, as `list_buildings`, `list_lots` and `query_cell` report them. Common occupant groups for
`list_buildings`: `0x1400` power, `0x1503` schools, `0x1507` health.

### Resources and prompts

| Resource | Content |
|---|---|
| `sc4://city/status`, `sc4://city/budget`, `sc4://city/demand` | Same as `get_city_status`, `get_budget`, `get_demand`. |
| `sc4://city/utilities`, `sc4://city/services`, `sc4://city/ordinances` | Same as `get_utilities`, `get_services`, `list_ordinances`. |
| `sc4://city/transport`, `sc4://city/loans`, `sc4://city/neighbors`, `sc4://city/growth` | Same as `get_transport`, `get_loans`, `get_neighbor_deals`, `diagnose_growth`. |

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
| `sc4://log/SC4SIM.log`, `sc4://log/SC4MCP.log` | The last 200 lines of the log. |

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
| `AllowControl` | `1` | `0` removes `set_speed`, `wait_months`, `save_city`, `set_funding`, `set_building_funding`, `set_tax` and `set_ordinance`. |
| `AllowCheats` | `1` | `0` removes `run_cheat`. |
| `AllowBuild` | `1` | `0` removes `place_zone`. |
| `ReadOnly` | `0` | `1` turns off `AllowControl`, `AllowCheats` and `AllowBuild` at once. |
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
- Every change to the city (speed, funding, taxes, ordinances, zoning, saving, cheats) and every confirmation is
  written to `SC4MCP.log`.
- `read_log` reads only `.log` files in the Plugins folder.

## Building from source

Visual Studio 2022 with the *Desktop development with C++* workload. The game is 32-bit, so the DLL must be built
for **x86 (Win32)**.

**Visual Studio**

1. Open `SC4MCP.sln`.
2. Select **Release | x86** and build.
3. The DLL is `bin\Release\SC4MCP.dll`.

**CMake**

```sh
cmake -S . -B build -A Win32
cmake --build build --config Release
```

All dependencies are vendored in `vendor/`; nothing else needs to be installed.

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
- add new tools to `src/ToolsRead.cpp` or `src/ToolsControl.cpp` with a JSON schema and the right permission, and
  document them in this README;
- test with SimCity 4 1.1.641 and describe the test in the pull request.

## Changelog

### 1.3.0

- New tools: `get_transport`, `get_loans`, `take_loan`, `get_neighbor_deals`, `render_map` (PNG images of the city
  maps), `list_changes`, `undo_last_change`.
- Notifications: a standalone stream (`GET /mcp`) with new-month messages, warnings and resource updates; resource
  subscriptions and `logging/setLevel`.
- Paging: `offset` and `next_offset` for `list_buildings` and `list_lots`.
- Every tool declares an output schema; four new resources.

### 1.2.0

- Diagnostics: `diagnose_growth` and `diagnose_cell` explain why the city or a cell does not grow, from the game's own
  state (demand, vacate reasons, development failures, power, road access, land value, pollution, crime).
- `get_building_properties`: the properties of a building or any exemplar as the game loaded them.
- Snapshots: `snapshot_save`, `snapshot_compare`, `snapshot_list`, `snapshot_delete`.
- Confirmations: tools that change the city ask the user through MCP elicitation, or return a preview and wait for
  `confirm=true`; all accept `dry_run=true`. New settings `ConfirmChanges` and `ReadOnly`; `AuthToken=auto`.
- `http://127.0.0.1:8765/setup`: the configuration of every AI client, ready to copy.
- Security: `Host` header check; one thread per connection.
- Fixed: `list_graphs` and `get_history` read only the first line of each graph (array properties were not detected).

### 1.1.0

- New read tools: `get_demand`, `get_taxes`, `get_utilities`, `get_services`, `list_lots`, `query_cell`,
  `get_map_stats`, `list_graphs`, `get_history`, `list_ordinances`, `get_advisors`, `get_game_data`.
- New control tools: `save_city`, `set_funding`, `set_building_funding`, `set_tax`, `set_ordinance`, `place_zone`
  (new setting `AllowBuild`).
- MCP resources and prompts; progress notifications for `wait_months`; optional bearer token (`AuthToken`).
- Fixed: `set_speed` reported the speed before the change; it now waits until the game has applied it.
- Fixed: demand values were read from index 0 for most types instead of the city totals (index 0x20000), and the
  demand cap of every type came from a value the game maintains only for agriculture; caps now come from the advisor
  data, and `cap_percent` is reported for I-R only.

### 1.0.0

- First release: `get_city_status`, `get_budget`, `list_buildings`, `read_log`, `set_speed`, `wait_months`,
  `run_cheat`.

## License

SC4MCP is released under the [MIT License](LICENSE).

Third-party code in `vendor/`:

| Component | License |
|---|---|
| [gzcom-dll](https://github.com/nsgomez/gzcom-dll) SDK (`vendor/gzcom-dll`) | LGPL 2.1, see `vendor/gzcom-dll/LICENSE` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (`vendor/nlohmann`) | MIT, see `vendor/nlohmann/LICENSE.MIT` |

SimCity 4 is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by Electronic Arts.
