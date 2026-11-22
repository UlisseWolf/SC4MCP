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
- [Connecting an MCP client](#connecting-an-mcp-client)
- [Configuration](#configuration)
- [Example session](#example-session)
- [How it works](#how-it-works)
- [Security](#security)
- [Building from source](#building-from-source)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)

## Features

- **Runs in the game process.** No external program, no memory scanning: the DLL uses the game's own COM interfaces
  through the [gzcom-dll](https://github.com/nsgomez/gzcom-dll) SDK.
- **Read the city.** Date, population, funds, income and expenses, RCI demand, every budget department with its line
  items, every building with its exemplar ID and monthly cost.
- **Drive the simulation.** Pause, change the speed, run for N game months and get the result back, run cheat codes
  (including the cheats registered by other DLL plugins).
- **Read plugin logs.** The last lines of any `.log` file in the Plugins folder, for example `SC4SIM.log`.
- **Safe by default.** Listens on `127.0.0.1` only, rejects non-local browser origins, and the control and cheat
  tools can be turned off in `SC4MCP.ini`.
- **Standard MCP.** Streamable HTTP transport, protocol versions `2025-06-18`, `2025-03-26` and `2024-11-05`.

SC4MCP does not place buildings, zones or networks: building the city stays with the player.

## Tools

| Tool | Parameters | Description | Requires |
|---|---|---|---|
| `get_city_status` | - | City name, date, pause state and speed, population, funds, monthly income and expense, RCI demand. | |
| `get_budget` | `per_building` (bool) | Funds and totals, every department with its funding percentage and line items. With `per_building=true`, every building with its budget items. | |
| `list_buildings` | `name_contains` (string), `occupant_group` (hex string, e.g. `0x1503`), `limit` (1-2000) | Buildings with exemplar ID, city cells and monthly cost. | |
| `read_log` | `file` (string, default `SC4SIM.log`), `lines` (1-1000) | The last lines of a `.log` file next to the DLL. | |
| `set_speed` | `speed` (0-4) | Pause (0) or run at speed 1 (turtle) to 4 (cheetah). | `AllowControl=1` |
| `wait_months` | `months` (1-120), `speed` (1-4), `timeout_seconds` (5-600, default 50) | Runs the simulation for N game months, then pauses and returns the city status. | `AllowControl=1` |
| `run_cheat` | `cheat` (string) | Runs a cheat code as if typed in the Ctrl+X box. | `AllowCheats=1` |

Common occupant groups for `list_buildings`: `0x1400` power, `0x1503` schools, `0x1507` health.

## Requirements

- SimCity 4 Deluxe or Rush Hour, **version 1.1.641** (Windows; the Steam and GOG versions are 641).
- Windows 10 or 11.
- An MCP client that supports the Streamable HTTP transport, or Node.js for the
  [`mcp-remote`](https://www.npmjs.com/package/mcp-remote) bridge (Claude Desktop).

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

### Other clients

Any client that speaks MCP over Streamable HTTP can connect to the same URL. Clients that only support stdio can use
`mcp-remote` as shown above.

## Configuration

`SC4MCP.ini` sits next to the DLL. Restart the game after a change.

| Key | Default | Description |
|---|---|---|
| `Enabled` | `1` | `0` turns the plugin off. |
| `Port` | `8765` | TCP port on `127.0.0.1`. Change it if another program uses 8765. |
| `AllowControl` | `1` | `0` removes `set_speed` and `wait_months` (read-only server). |
| `AllowCheats` | `1` | `0` removes `run_cheat`. |

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

| Source file | Role |
|---|---|
| `src/McpDllDirector.cpp` | DLL entry point (COM director): reads the INI, starts the server and the tick service. |
| `src/HttpServer.cpp` | Minimal HTTP/1.1 server on a worker thread, origin check. |
| `src/McpServer.cpp` | JSON-RPC 2.0 / MCP: `initialize`, `tools/list`, `tools/call`, `/health`. |
| `src/MainThreadQueue.cpp`, `src/TickService.cpp` | Hand-off of requests to the game thread. |
| `src/Tools.cpp` | The tools. |
| `src/McpLog.cpp` | `SC4MCP.log`. |

## Security

- The server binds to `127.0.0.1` only and is never reachable from other machines.
- Requests whose `Origin` header is not a local address are refused (protection against DNS rebinding from web pages).
- There is no authentication: any program running on the same PC can call the server while the game is open. Set
  `AllowControl=0` and `AllowCheats=0` for a read-only server, or `Enabled=0` when you do not need it.
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
| `http://127.0.0.1:8765/health` does not open | The game is not running, or `Enabled=0`. |

## Contributing

Issues and pull requests are welcome. Please:

- keep every game call on the game thread (through `MainThreadQueue`);
- add new tools to `src/Tools.cpp` with a JSON schema for their parameters, and document them in this README;
- test with SimCity 4 1.1.641 and describe the test in the pull request.

## License

SC4MCP is released under the [MIT License](LICENSE).

Third-party code in `vendor/`:

| Component | License |
|---|---|
| [gzcom-dll](https://github.com/nsgomez/gzcom-dll) SDK (`vendor/gzcom-dll`) | LGPL 2.1, see `vendor/gzcom-dll/LICENSE` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 (`vendor/nlohmann`) | MIT, see `vendor/nlohmann/LICENSE.MIT` |

SimCity 4 is a trademark of Electronic Arts Inc. This project is not affiliated with or endorsed by Electronic Arts.
