// SC4MCP - transport, loans, neighbour deals, map images, and the change history with undo.
#include "ToolsCommon.h"
#include "McpLog.h"

#include "cISC4BudgetSimulator.h"
#include "cISC4City.h"
#include "cISC4CrimeSimulator.h"
#include "cISC4FireProtectionSimulator.h"
#include "cISC4LandValueSimulator.h"
#include "cISC4Lot.h"
#include "cISC4LotManager.h"
#include "cISC4Occupant.h"
#include "SC4Rect.h"
#include "cISC4NeighborDeal.h"
#include "cISC4NeighborsSimulator.h"
#include "cISC4PoliceSimulator.h"
#include "cISC4PollutionSimulator.h"
#include "cISC4PowerSimulator.h"
#include "cISC4RegionalCity.h"
#include "cISC4SimGrid.h"
#include "cISC4TrafficSimulator.h"
#include "cISC4ZoneDeveloper.h"
#include "cISC4ZoneManager.h"
#include "cRZAutoRefCount.h"
#include "cRZBaseString.h"
#include "SC4List.h"
#include "SC4Vector.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>

using nlohmann::json;
using namespace ToolsCommon;

// ===================================================================================================== change history

namespace
{
	struct ChangeEntry
	{
		std::string description;
		std::string time;
		json undo;
	};
	std::mutex changeMutex;
	std::deque<ChangeEntry> changes;
	std::atomic<bool> recordingSuppressed{ false };
	constexpr size_t kMaxChanges = 50;

	std::string ClockTime()
	{
		const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
		char text[32];
		std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
		return text;
	}
}

namespace ToolsCommon
{
	void RecordChange(const std::string& description, json undoCalls)
	{
		if (recordingSuppressed) return;
		std::lock_guard<std::mutex> lock(changeMutex);
		changes.push_back({ description, ClockTime(), std::move(undoCalls) });
		while (changes.size() > kMaxChanges) changes.pop_front();
	}
}

// ===================================================================================================== PNG

namespace
{
	uint32_t Crc32(const uint8_t* data, size_t length, uint32_t crc = 0)
	{
		static const auto table = []
		{
			std::array<uint32_t, 256> t{};
			for (uint32_t n = 0; n < 256; ++n)
			{
				uint32_t c = n;
				for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				t[n] = c;
			}
			return t;
		}();
		crc = ~crc;
		for (size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	void Put32(std::vector<uint8_t>& out, uint32_t value)
	{
		out.push_back(static_cast<uint8_t>(value >> 24));
		out.push_back(static_cast<uint8_t>(value >> 16));
		out.push_back(static_cast<uint8_t>(value >> 8));
		out.push_back(static_cast<uint8_t>(value));
	}

	void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
	{
		Put32(out, static_cast<uint32_t>(data.size()));
		std::vector<uint8_t> body(type, type + 4);
		body.insert(body.end(), data.begin(), data.end());
		out.insert(out.end(), body.begin(), body.end());
		Put32(out, Crc32(body.data(), body.size()));
	}

	std::string Base64(const std::vector<uint8_t>& data)
	{
		static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string out;
		out.reserve((data.size() + 2) / 3 * 4);
		for (size_t i = 0; i < data.size(); i += 3)
		{
			const uint32_t a = data[i], b = i + 1 < data.size() ? data[i + 1] : 0, c = i + 2 < data.size() ? data[i + 2] : 0;
			const uint32_t triple = (a << 16) | (b << 8) | c;
			out += alphabet[(triple >> 18) & 63];
			out += alphabet[(triple >> 12) & 63];
			out += i + 1 < data.size() ? alphabet[(triple >> 6) & 63] : '=';
			out += i + 2 < data.size() ? alphabet[triple & 63] : '=';
		}
		return out;
	}
}

namespace ToolsCommon
{
	std::string EncodePngBase64(int width, int height, const std::vector<uint8_t>& pixels, const std::vector<uint32_t>& palette)
	{
		std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
		std::vector<uint8_t> header;
		Put32(header, static_cast<uint32_t>(width));
		Put32(header, static_cast<uint32_t>(height));
		header.insert(header.end(), { 8, 3, 0, 0, 0 }); // 8-bit, palette, deflate, adaptive filter, no interlace
		Chunk(png, "IHDR", header);
		std::vector<uint8_t> plte;
		for (uint32_t rgb : palette) { plte.push_back(static_cast<uint8_t>(rgb >> 16)); plte.push_back(static_cast<uint8_t>(rgb >> 8)); plte.push_back(static_cast<uint8_t>(rgb)); }
		Chunk(png, "PLTE", plte);

		// Raw scanlines (filter 0), stored in uncompressed deflate blocks inside a zlib stream.
		std::vector<uint8_t> raw;
		raw.reserve(static_cast<size_t>(height) * (width + 1));
		for (int y = 0; y < height; ++y)
		{
			raw.push_back(0);
			raw.insert(raw.end(), pixels.begin() + static_cast<size_t>(y) * width, pixels.begin() + static_cast<size_t>(y + 1) * width);
		}
		std::vector<uint8_t> zlib = { 0x78, 0x01 };
		for (size_t offset = 0; offset < raw.size() || offset == 0; )
		{
			const size_t length = std::min<size_t>(65535, raw.size() - offset);
			const bool last = offset + length >= raw.size();
			zlib.push_back(last ? 1 : 0);
			zlib.push_back(static_cast<uint8_t>(length));
			zlib.push_back(static_cast<uint8_t>(length >> 8));
			zlib.push_back(static_cast<uint8_t>(~length));
			zlib.push_back(static_cast<uint8_t>(~length >> 8));
			zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + length);
			offset += length;
			if (last) break;
		}
		uint32_t a = 1, b = 0;
		for (uint8_t byte : raw) { a = (a + byte) % 65521; b = (b + a) % 65521; }
		Put32(zlib, (b << 16) | a);
		Chunk(png, "IDAT", zlib);
		Chunk(png, "IEND", {});
		return Base64(png);
	}
}

// ===================================================================================================== tools

namespace
{
	// ------------------------------------------------------------------------------------------------- transport

	json GetTransport()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		json out = json::object();
		GameTable table;
		if (ReadGameTable(city, table))
		{
			out["advisor_data"] = GameFields(table, { "commute", "transit", "bus", "subway", "rail", "train", "station", "congestion", "traffic",
				"pothole", "ferry", "airport", "seaport", "monorail", "highway", "road", "_el_", "trip", "car" });
		}
		if (cISC4TrafficSimulator* traffic = city->GetTrafficSimulator())
		{
			out["trip_scale"] = traffic->GetTripScaleForDisplay();
			if (cISC4SimGrid<uint8_t>* congestion = traffic->GetCongestionMap())
			{
				const int32_t sizeX = static_cast<int32_t>(city->CellCountX()), sizeZ = static_cast<int32_t>(city->CellCountZ());
				long long sum = 0;
				int cells = 0, heavy = 0, maximum = 0;
				for (int32_t x = 0; x < sizeX; ++x)
				{
					for (int32_t z = 0; z < sizeZ; ++z)
					{
						const int value = congestion->GetCellValue(x, z);
						if (value <= 0) continue;
						sum += value;
						++cells;
						if (value >= 128) ++heavy;
						maximum = std::max(maximum, value);
					}
				}
				out["congestion_map"] = json{ {"cells_with_traffic", cells}, {"average", cells ? static_cast<double>(sum) / cells : 0.0},
					{"maximum", maximum}, {"heavy_cells", heavy}, {"note", "0-255 per cell; heavy = 128 or more. render_map map=congestion draws it."} };
			}
		}
		return out;
	}

	// ------------------------------------------------------------------------------------------------- loans

	json GetLoans()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		if (!budget) return Error("Budget simulator unavailable.");
		SC4Vector<cISC4BudgetSimulator::LoanInfo> loans;
		budget->GetAllLoans(loans);
		json list = json::array();
		for (const auto& loan : loans)
		{
			list.push_back(json{ {"amount", loan.totalLoanAmount}, {"remaining_payment", loan.remainingPayment}, {"monthly_payment", loan.monthlyPayment},
				{"months_remaining", loan.monthsRemaining}, {"years_remaining", loan.yearsRemaining} });
		}
		const int64_t increment = budget->GetBondIncrement();
		return json{ {"loans", list}, {"total_borrowed", budget->GetTotalBorrowed()}, {"monthly_payments", budget->GetTotalMonthlyBondPayments()},
			{"borrowing_limit", budget->GetCurrentBorrowingLimit()}, {"available_now", budget->GetCurrentBondLimit()},
			{"max_outstanding_loans", budget->GetCurrentMaxOutstandingBondsLimit()}, {"loan_step", increment},
			{"loan_months", budget->GetLoanTimeInMonths()},
			{"example", increment > 0 ? json{ {"amount", increment}, {"monthly_payment", budget->GetMonthlyPaymentForLoan(increment)}, {"full_cost", budget->GetFullCostOfLoan(increment)} } : json(nullptr)},
			{"funds", budget->GetTotalFunds()} };
	}

	int64_t LoanAmount(cISC4BudgetSimulator* budget, double requested)
	{
		const int64_t step = std::max<int64_t>(1, budget->GetBondIncrement());
		const int64_t amount = static_cast<int64_t>(std::llround(requested / static_cast<double>(step))) * step;
		return std::max<int64_t>(step, amount);
	}

	json PreviewLoan(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		if (!budget) return Error("Budget simulator unavailable.");
		const int64_t amount = LoanAmount(budget, arguments.at("amount").get<double>());
		const int64_t available = budget->GetCurrentBondLimit();
		if (amount > available) return Error("The city can borrow at most " + std::to_string(available) + " now (get_loans).");
		return json{ {"action", "take_loan"}, {"amount", amount}, {"monthly_payment", budget->GetMonthlyPaymentForLoan(amount)},
			{"full_cost", budget->GetFullCostOfLoan(amount)}, {"months", budget->GetLoanTimeInMonths()}, {"funds_now", budget->GetTotalFunds()},
			{"note", "A loan cannot be undone."} };
	}

	json TakeLoan(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4BudgetSimulator* budget = city->GetBudgetSimulator();
		if (!budget) return Error("Budget simulator unavailable.");
		const int64_t amount = LoanAmount(budget, arguments.at("amount").get<double>());
		if (amount > budget->GetCurrentBondLimit()) return Error("Amount above the current borrowing limit.");
		const int64_t before = budget->GetTotalFunds();
		const bool ok = budget->IssueBond(static_cast<uint32_t>(amount));
		McpLog::Write("take_loan: %lld %s", static_cast<long long>(amount), ok ? "issued" : "refused");
		if (ok) RecordChange("Loan of " + std::to_string(amount), json::array());
		return json{ {"amount", amount}, {"ok", ok}, {"funds_before", before}, {"funds_after", budget->GetTotalFunds()},
			{"monthly_payments", budget->GetTotalMonthlyBondPayments()} };
	}

	// ------------------------------------------------------------------------------------------------- neighbours

	std::string RegionalCityName(cISC4RegionalCity* city)
	{
		if (!city) return {};
		cRZBaseString name;
		city->GetCityName(name);
		return name.ToChar();
	}

	json GetNeighborDeals()
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		cISC4NeighborsSimulator* neighbors = city->GetNeighborsSimulator();
		if (!neighbors) return Error("Neighbours simulator unavailable.");
		json deals = json::array();
		{
			SC4List<cRZAutoRefCount<cISC4NeighborDeal>> list;
			neighbors->FindDealsAll(list);
			for (const auto& entry : list)
			{
				cISC4NeighborDeal* deal = entry;
				if (!deal) continue;
				deals.push_back(json{ {"type", deal->GetType()}, {"city", RegionalCityName(deal->GetCityOnOtherSideOfConnection())},
					{"amount", deal->GetAmount()}, {"last_amount", deal->GetLastAmount()}, {"amount_used", deal->GetAmountUsed()},
					{"monthly_cost", deal->GetCost()} });
			}
		}
		json adjacent = json::array();
		{
			SC4List<cRZAutoRefCount<cISC4RegionalCity>> list;
			neighbors->GetAdjacentCities(list, false);
			for (const auto& entry : list)
			{
				cISC4RegionalCity* neighbour = entry;
				if (neighbour) adjacent.push_back(RegionalCityName(neighbour));
			}
		}
		json out{ {"deals", deals}, {"monthly_income", neighbors->GetTotalMonthlyNeighborDealIncome()},
			{"monthly_expense", neighbors->GetTotalMonthlyNeighborDealExpense()}, {"adjacent_cities", adjacent},
			{"note", "type is the game's deal type number (power, water, garbage); a positive monthly_cost is paid by this city."} };
		GameTable table;
		if (ReadGameTable(city, table)) out["advisor_data"] = GameFields(table, { "nd_", "neighbor", "neighbour", "deal" });
		return out;
	}

	// ------------------------------------------------------------------------------------------------- map images

	constexpr uint8_t kWater = 0, kNoData = 1, kRampFirst = 2, kRampSteps = 32, kZoneFirst = kRampFirst + kRampSteps, kHighlight = kZoneFirst + 16;

	// Marks the lots that match highlight: abandoned, unpowered, no_road, an occupant group ("0x1503") or
	// "name:<text>". Returns how many lots were marked, or -1 for an unknown highlight.
	int Highlight(cISC4City* city, const std::string& what, std::vector<uint8_t>& cells, int32_t sizeX, int32_t sizeZ)
	{
		cISC4LotManager* lots = city->GetLotManager();
		if (!lots) return 0;
		uint32_t group = 0;
		std::string needle;
		if (what.rfind("name:", 0) == 0) needle = Lower(what.substr(5));
		else if (what != "abandoned" && what != "unpowered" && what != "no_road")
		{
			try { group = ParseId(what); } catch (...) { return -1; }
		}
		int marked = 0;
		for (cISC4Occupant* occupant : AllBuildings(city))
		{
			cISC4Lot* lot = lots->GetOccupantLot(occupant);
			if (!lot) continue;
			bool match = false;
			const int32_t zone = static_cast<int32_t>(lot->GetZoneType());
			if (what == "abandoned") match = lot->GetState() == cISC4Lot::HabitationState::Vacant;
			else if (what == "unpowered") match = lot->GetNeedsPower() && !lot->GetHasPower();
			else if (what == "no_road") match = zone >= 1 && zone <= 9 && !lot->HasRoadAccess();
			else if (!needle.empty()) match = Lower(BuildingName(occupant)).find(needle) != std::string::npos;
			else match = occupant->IsOccupantGroup(group);
			if (!match) continue;
			SC4Rect<int32_t> rect{};
			if (!lot->GetBoundingRect(rect)) continue;
			for (int32_t x = std::max(0, rect.topLeftX); x <= std::min(sizeX - 1, rect.bottomRightX); ++x)
				for (int32_t z = std::max(0, rect.topLeftY); z <= std::min(sizeZ - 1, rect.bottomRightY); ++z)
					cells[static_cast<size_t>(z) * sizeX + x] = kHighlight;
			++marked;
		}
		return marked;
	}

	uint32_t Mix(uint32_t a, uint32_t b, double t)
	{
		auto channel = [&](int shift) { return static_cast<uint32_t>(std::lround(((a >> shift) & 255) * (1 - t) + ((b >> shift) & 255) * t)) << shift; };
		return channel(16) | channel(8) | channel(0);
	}

	std::vector<uint32_t> Palette()
	{
		std::vector<uint32_t> palette = { 0x3A7BD5, 0xD8D8D8 };
		const uint32_t good = 0x2E9E6A, middle = 0xF2C94C, bad = 0xD64545;
		for (int i = 0; i < kRampSteps; ++i)
		{
			const double t = static_cast<double>(i) / (kRampSteps - 1);
			palette.push_back(t < 0.5 ? Mix(good, middle, t * 2) : Mix(middle, bad, (t - 0.5) * 2));
		}
		static const uint32_t zones[16] = { 0xE8E8E8, 0xB7E4C7, 0x52B788, 0x1B7F4F, 0xA9C9F5, 0x4F8FE0, 0x1F5FB8, 0xD4B483,
			0xF2C94C, 0xE8833A, 0x8A8984, 0x9B6FD1, 0x2A9DB8, 0x5C5C8A, 0x8B5E3C, 0xC94F9B };
		palette.insert(palette.end(), std::begin(zones), std::end(zones));
		palette.push_back(0xFF00FF); // highlight
		return palette;
	}

	json RenderMap(const json& arguments)
	{
		cISC4City* city = City();
		if (!city) return NoCity();
		const std::string name = Lower(arguments.at("map").get<std::string>());
		const int32_t sizeX = static_cast<int32_t>(city->CellCountX()), sizeZ = static_cast<int32_t>(city->CellCountZ());
		const int scale = std::clamp(arguments.value("scale", 512 / std::max(sizeX, sizeZ)), 1, 8);
		cISC4ZoneDeveloper* zoneDeveloper = city->GetZoneDeveloper();
		std::vector<uint8_t> cellsIndex(static_cast<size_t>(sizeX) * sizeZ, kNoData);
		json legend = json::array();
		json out{ {"map", name} };

		if (name == "zones")
		{
			cISC4ZoneManager* zones = city->GetZoneManager();
			if (!zones) return Error("Zone manager unavailable.");
			for (int32_t z = 0; z < sizeZ; ++z)
			{
				for (int32_t x = 0; x < sizeX; ++x)
				{
					const int32_t zone = static_cast<int32_t>(zones->GetZoneType(x, z));
					uint8_t index = static_cast<uint8_t>(kZoneFirst + std::clamp(zone, 0, 15));
					if (zone == 0 && zoneDeveloper && zoneDeveloper->IsCellWater(x, z)) index = kWater;
					cellsIndex[static_cast<size_t>(z) * sizeX + x] = index;
				}
			}
			const std::vector<uint32_t> palette = Palette();
			for (int32_t zone = 0; zone <= 15; ++zone)
			{
				char color[8];
				std::snprintf(color, sizeof(color), "#%06X", palette[kZoneFirst + zone]);
				legend.push_back(json{ {"color", color}, {"meaning", ZoneName(zone)} });
			}
		}
		else
		{
			const MapReader* map = FindMapReader(name);
			if (!map)
			{
				json names = json::array({ "zones" });
				for (const MapReader& candidate : MapReaders()) names.push_back(candidate.name);
				return json{ {"error", "Unknown map."}, {"maps", names} };
			}
			std::vector<double> values(cellsIndex.size(), std::nan(""));
			double minimum = 1e300, maximum = -1e300;
			for (int32_t z = 0; z < sizeZ; ++z)
			{
				for (int32_t x = 0; x < sizeX; ++x)
				{
					double value = 0;
					if (!map->read(city, x, z, value) || !std::isfinite(value)) continue;
					values[static_cast<size_t>(z) * sizeX + x] = value;
					minimum = std::min(minimum, value);
					maximum = std::max(maximum, value);
				}
			}
			const bool fixedRange = arguments.value("fixed_range", true);
			const double low = fixedRange ? 0.0 : minimum, high = fixedRange ? (name == "powered" ? 1.0 : std::max(255.0, maximum)) : maximum;
			for (size_t i = 0; i < values.size(); ++i)
			{
				const int32_t x = static_cast<int32_t>(i % sizeX), z = static_cast<int32_t>(i / sizeX);
				if (zoneDeveloper && zoneDeveloper->IsCellWater(x, z) && name != "water_pollution") { cellsIndex[i] = kWater; continue; }
				if (std::isnan(values[i])) continue;
				double t = high > low ? (values[i] - low) / (high - low) : 0.0;
				t = std::clamp(t, 0.0, 1.0);
				if (!map->higherIsWorse) t = 1.0 - t;
				cellsIndex[i] = static_cast<uint8_t>(kRampFirst + std::lround(t * (kRampSteps - 1)));
			}
			out["min"] = minimum;
			out["max"] = maximum;
			out["range"] = json{ {"low", low}, {"high", high} };
			legend.push_back(json{ {"color", "green to red"}, {"meaning", map->higherIsWorse ? "low (good) to high (bad)" : "high (good) to low (bad)"} });
		}
		if (arguments.contains("highlight") && arguments["highlight"].is_string())
		{
			const std::string what = Lower(arguments["highlight"].get<std::string>());
			const int marked = Highlight(city, what, cellsIndex, sizeX, sizeZ);
			if (marked < 0) return Error("highlight must be abandoned, unpowered, no_road, an occupant group (e.g. 0x1503) or name:<text>.");
			out["highlighted_lots"] = marked;
			legend.push_back(json{ {"color", "#FF00FF"}, {"meaning", "highlighted: " + what} });
		}
		legend.push_back(json{ {"color", "#3A7BD5"}, {"meaning", "water"} });
		legend.push_back(json{ {"color", "#D8D8D8"}, {"meaning", "no data"} });

		const int width = sizeX * scale, height = sizeZ * scale;
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height);
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x) pixels[static_cast<size_t>(y) * width + x] = cellsIndex[static_cast<size_t>(y / scale) * sizeX + x / scale];
		}
		out["width"] = width;
		out["height"] = height;
		out["scale"] = scale;
		out["orientation"] = "Cell (0, 0) is the top-left corner; x grows to the right, z downwards. One cell is scale x scale pixels.";
		out["legend"] = legend;
		out["_image"] = json{ {"data", EncodePngBase64(width, height, pixels, Palette())}, {"mimeType", "image/png"} };
		return out;
	}

	// ------------------------------------------------------------------------------------------------- undo

	json ListChanges()
	{
		std::lock_guard<std::mutex> lock(changeMutex);
		json list = json::array();
		for (auto it = changes.rbegin(); it != changes.rend(); ++it)
		{
			list.push_back(json{ {"time", it->time}, {"change", it->description}, {"can_undo", !it->undo.empty()} });
		}
		return json{ {"changes", list}, {"note", "Newest first; kept in memory until the game closes. Zoning and loans cannot be undone."} };
	}

	json UndoLastChange(const json& arguments, bool& isError, const Tools::Context& context)
	{
		ChangeEntry entry;
		{
			std::lock_guard<std::mutex> lock(changeMutex);
			auto it = std::find_if(changes.rbegin(), changes.rend(), [](const ChangeEntry& e) { return !e.undo.empty(); });
			if (it == changes.rend()) { isError = true; return Error("No change that can be undone (list_changes)."); }
			entry = *it;
		}
		json plan{ {"action", "undo_last_change"}, {"change", entry.description}, {"made_at", entry.time}, {"calls", entry.undo} };
		if (arguments.value("dry_run", false)) { plan["dry_run"] = true; return plan; }
		if (ConfirmChangesEnabled() && !arguments.value("confirm", false))
		{
			const int answer = context.confirm ? context.confirm("Undo \"" + entry.description + "\"?") : -1;
			if (answer == 0) { plan["cancelled"] = true; return plan; }
			if (answer < 0) { plan["confirmation_required"] = true; plan["note"] = "Nothing was changed. Call again with confirm=true to undo it."; return plan; }
		}
		json results = json::array();
		recordingSuppressed = true;
		for (const json& call : entry.undo)
		{
			json callArguments = call.value("arguments", json::object());
			callArguments["confirm"] = true;
			bool callError = false;
			results.push_back(CallTool(call.value("tool", std::string()), callArguments, callError));
			if (callError) isError = true;
		}
		recordingSuppressed = false;
		{
			std::lock_guard<std::mutex> lock(changeMutex);
			auto it = std::find_if(changes.rbegin(), changes.rend(), [&](const ChangeEntry& e) { return e.time == entry.time && e.description == entry.description; });
			if (it != changes.rend()) changes.erase(std::next(it).base());
		}
		McpLog::Write("undo_last_change: %s", entry.description.c_str());
		return json{ {"undone", entry.description}, {"results", results} };
	}
}

namespace ToolsCommon
{
	void AddExtraTools(std::vector<ToolDef>& tools)
	{
		tools.push_back({ "get_transport",
			"Transport and commuting: the commute, transit (bus, rail, subway, monorail, ferry, airport, seaport), station use, congestion and pothole fields of the advisor data, and statistics of the congestion map.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetTransport(); }) });

		tools.push_back({ "get_loans",
			"Loans: every loan with its monthly payment and remaining time, total borrowed, what the city can borrow now, the loan step and the cost of a loan.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetLoans(); }) });

		tools.push_back({ "take_loan",
			"Takes a loan (the amount is rounded to the game's loan step and must be within get_loans available_now). Cannot be undone. Asks for confirmation; dry_run=true only previews.",
			Schema({ {"amount", {{"type", "number"}, {"minimum", 1}}}, {"dry_run", {{"type", "boolean"}}}, {"confirm", {{"type", "boolean"}}} }, json::array({ "amount" })), Permission::Control,
			Guarded(PreviewLoan, TakeLoan,
				[](const json& p) { return "Borrow " + std::to_string(p.value("amount", 0LL)) + " for " + std::to_string(p.value("monthly_payment", 0LL)) + " a month?"; }) });

		tools.push_back({ "get_neighbor_deals",
			"Neighbour deals (power, water, garbage bought or sold): city, amount, use and monthly cost of each deal, the monthly income and expense of all deals, and the adjacent cities.",
			Schema(json::object()), Permission::Read, GameThread([](const json&) { return GetNeighborDeals(); }) });

		tools.push_back({ "render_map",
			"Draws a city map as a PNG image (returned as an image the assistant can look at): zones, land_value, crime, air_pollution, water_pollution, garbage, fire_protection, police_coverage, congestion or powered. Green is good, red is bad; water is blue. scale: pixels per cell (default so the image is about 512 pixels). fixed_range=false stretches the colours to the values present.",
			Schema({ {"map", {{"type", "string"}}}, {"scale", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}}, {"fixed_range", {{"type", "boolean"}}},
				{"highlight", {{"type", "string"}, {"description", "Mark lots in magenta: abandoned, unpowered, no_road, an occupant group (e.g. \"0x1503\" schools) or name:<text>."}}} }, json::array({ "map" })), Permission::Read,
			GameThread([](const json& arguments) { return RenderMap(arguments); }) });

		tools.push_back({ "list_changes",
			"The changes the tools made in this session (funding, taxes, ordinances, loans), newest first, and whether each can be undone.",
			Schema(json::object()), Permission::Read,
			[](const json&, bool&, const Tools::Context&) { return ListChanges(); } });

		tools.push_back({ "undo_last_change",
			"Undoes the last change that can be undone (funding, taxes or ordinances) by restoring the previous values. Asks for confirmation; dry_run=true only previews.",
			Schema({ {"dry_run", {{"type", "boolean"}}}, {"confirm", {{"type", "boolean"}}} }), Permission::Control,
			[](const json& arguments, bool& isError, const Tools::Context& context) { return UndoLastChange(arguments, isError, context); } });
	}
}
