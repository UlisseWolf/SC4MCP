# Changelog

All notable changes to SC4MCP are listed here, newest first. Versions follow
[Semantic Versioning](https://semver.org).

## 1.4.0

- License: SC4MCP is now released under the GNU Lesser General Public License, version 2.1 (it was MIT).
- GitHub workflows, after [sc4-seamless-region](https://github.com/caspervg/sc4-seamless-region): build on
  `master`, attested releases from `v*` tags on `master`, sc4pac update when a release is published, Dependabot.
- The gzcom-dll SDK is now the [SC4SDK](https://github.com/SC4SDK/gzcom-dll) version, as a git submodule. Its
  corrected signatures fix the monthly upkeep of jails (now the sum of every jail, also shown per jail).
- Players: `suggest_location` (where to build schools, hospitals, fire and police stations), `tax_what_if`,
  `get_region`, `export_csv`, and `render_map highlight`.
- Modders: `read_resource` (with inherited cohort properties), `audit_buildings`, `list_plugins`,
  `find_plugin_conflicts` and `find_resource_file` (DBPF index scan, after
  [dbpf-mcp](https://github.com/caspervg/dbpf-mcp)), `list_cheats` and `run_scenario`.
- Snapshots are saved to disk and survive a restart of the game (at most 30).
- `get_building_properties` takes property names from `new_properties.xml` when available, and reads cohorts too.

## 1.3.0

- New tools: `get_transport`, `get_loans`, `take_loan`, `get_neighbor_deals`, `render_map` (PNG images of the city
  maps), `list_changes`, `undo_last_change`.
- Notifications: a standalone stream (`GET /mcp`) with new-month messages, warnings and resource updates; resource
  subscriptions and `logging/setLevel`.
- Paging: `offset` and `next_offset` for `list_buildings` and `list_lots`.
- Every tool declares an output schema; four new resources.

## 1.2.0

- Diagnostics: `diagnose_growth` and `diagnose_cell` explain why the city or a cell does not grow, from the game's own
  state (demand, vacate reasons, development failures, power, road access, land value, pollution, crime).
- `get_building_properties`: the properties of a building or any exemplar as the game loaded them.
- Snapshots: `snapshot_save`, `snapshot_compare`, `snapshot_list`, `snapshot_delete`.
- Confirmations: tools that change the city ask the user through MCP elicitation, or return a preview and wait for
  `confirm=true`; all accept `dry_run=true`. New settings `ConfirmChanges` and `ReadOnly`; `AuthToken=auto`.
- `http://127.0.0.1:8765/setup`: the configuration of every AI client, ready to copy.
- Security: `Host` header check; one thread per connection.
- Fixed: `list_graphs` and `get_history` read only the first line of each graph (array properties were not detected).

## 1.1.0

- New read tools: `get_demand`, `get_taxes`, `get_utilities`, `get_services`, `list_lots`, `query_cell`,
  `get_map_stats`, `list_graphs`, `get_history`, `list_ordinances`, `get_advisors`, `get_game_data`.
- New control tools: `save_city`, `set_funding`, `set_building_funding`, `set_tax`, `set_ordinance`, `place_zone`
  (new setting `AllowBuild`).
- MCP resources and prompts; progress notifications for `wait_months`; optional bearer token (`AuthToken`).
- Fixed: `set_speed` reported the speed before the change; it now waits until the game has applied it.
- Fixed: demand values were read from index 0 for most types instead of the city totals (index 0x20000), and the
  demand cap of every type came from a value the game maintains only for agriculture; caps now come from the advisor
  data, and `cap_percent` is reported for I-R only.

## 1.0.0

- First release: `get_city_status`, `get_budget`, `list_buildings`, `read_log`, `set_speed`, `wait_months`,
  `run_cheat`.
