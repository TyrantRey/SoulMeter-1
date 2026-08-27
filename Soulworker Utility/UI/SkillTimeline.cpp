#include "pch.h"
#include "SkillTimeline.h"

#include ".\Combat Meter\Combat.h"
#include ".\Combat Meter\CombatMeter.h"
#include ".\Damage Meter\Damage Meter.h"
#include ".\Damage Meter\MySQLite.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
	constexpr float kPlotHeight = 260.0f;
	constexpr float kHoverRadiusPx = 8.0f;

	// Raw entry copied out of the combat log while its lock is held; names are
	// resolved afterwards so no other lock is taken under the combat lock.
	struct RawEntry {
		uint32_t _id;
		bool _isPlayer;
		uint32_t _skillId;
		double _elapsedMs;   // raid-timer time, 0 when unknown
		uint64_t _timestamp; // wall clock, ms
	};

	std::string CsvQuote(const std::string& s)
	{
		std::string out = "\"";
		for (char c : s) {
			if (c == '"')
				out += '"';
			out += c;
		}
		out += '"';
		return out;
	}
}

void SkillTimeline::Clear()
{
	_lanes.clear();
	_entries.clear();
	_snapshotSource = nullptr;
	_snapshotCount = 0;
	_selectedLane = -1;
}

const char* SkillTimeline::SkillName(uint32_t skillId)
{
	auto found = _skillNameCache.find(skillId);
	if (found != _skillNameCache.end())
		return found->second.c_str();

	char buf[128] = { 0 };
	bool ok = SWDB.GetSkillName(skillId, buf, sizeof(buf));
	if (!ok || buf[0] == 0 || strcmp(buf, "0") == 0)
		sprintf_s(buf, "%u", skillId);

	return _skillNameCache.emplace(skillId, buf).first->second.c_str();
}

void SkillTimeline::RebuildIfNeeded()
{
	CombatInterface* ci = COMBATMETER.Get();
	if (ci == nullptr) {
		if (_snapshotSource != nullptr)
			Clear();
		return;
	}

	size_t count = 0;
	COMBATMETER.GetLock();
	{
		for (auto itr = ci->begin(); itr != ci->end(); itr++)
			count += itr->second->size();
		COMBATMETER.FreeLock();
	}

	if (ci == _snapshotSource && count == _snapshotCount)
		return;

	Rebuild();
	_snapshotSource = ci;
	_snapshotCount = count;
}

void SkillTimeline::Rebuild()
{
	std::vector<RawEntry> raws;

	CombatInterface* ci = COMBATMETER.Get();
	if (ci == nullptr) {
		Clear();
		return;
	}

	COMBATMETER.GetLock();
	{
		for (auto itr = ci->begin(); itr != ci->end(); itr++) {
			Combat* pCombat = itr->second;
			bool isPlayer = pCombat->GetType() == CombatType::PLAYER;

			for (auto log = pCombat->begin(); log != pCombat->end(); log++) {
				CombatLog* pLog = log->second;
				if (pLog->_type != CombatLogType::USED_SKILL)
					continue;

				RawEntry raw;
				raw._id = pCombat->GetID();
				raw._isPlayer = isPlayer;
				raw._skillId = static_cast<uint32_t>(pLog->_val1);
				raw._elapsedMs = pLog->_val2;
				raw._timestamp = log->first;
				raws.push_back(raw);
			}
		}
		COMBATMETER.FreeLock();
	}

	// Time base. Entries stamped with the raid timer are exact; anything else
	// (older histories, casts before the timer started) is placed by wall clock
	// relative to the same origin, which puts pre-pull casts at negative times.
	double originTs = 0;
	bool haveOrigin = false;
	uint64_t minTs = UINT64_MAX;
	for (const RawEntry& raw : raws) {
		if (!haveOrigin && raw._elapsedMs > 0) {
			originTs = static_cast<double>(raw._timestamp) - raw._elapsedMs;
			haveOrigin = true;
		}
		minTs = (raw._timestamp < minTs) ? raw._timestamp : minTs;
	}
	if (!haveOrigin && !raws.empty())
		originTs = static_cast<double>(minTs);

	struct Timed {
		RawEntry _raw;
		double _seconds;
	};
	std::vector<Timed> timed;
	timed.reserve(raws.size());
	for (const RawEntry& raw : raws) {
		double seconds = (raw._elapsedMs > 0)
			? raw._elapsedMs / 1000.0
			: (static_cast<double>(raw._timestamp) - originTs) / 1000.0;
		timed.push_back({ raw, seconds });
	}
	std::stable_sort(timed.begin(), timed.end(), [](const Timed& a, const Timed& b) {
		return a._seconds < b._seconds;
	});

	// Lanes: players in order of first cast, then bosses.
	std::vector<Lane> lanes;
	std::unordered_map<uint64_t, size_t> laneIndex;
	auto laneKey = [](uint32_t id, bool isPlayer) { return (static_cast<uint64_t>(isPlayer) << 32) | id; };

	for (int pass = 0; pass < 2; pass++) {
		bool wantPlayer = (pass == 0);
		for (const Timed& t : timed) {
			if (t._raw._isPlayer != wantPlayer)
				continue;
			uint64_t key = laneKey(t._raw._id, t._raw._isPlayer);
			if (laneIndex.find(key) != laneIndex.end())
				continue;

			Lane lane;
			lane._id = t._raw._id;
			lane._isPlayer = t._raw._isPlayer;
			if (t._raw._isPlayer) {
				lane._name = DAMAGEMETER.GetPlayerName(t._raw._id);
			}
			else {
				char buf[256] = { 0 };
				if (SWDB.GetMonsterName(t._raw._id, buf, sizeof(buf)) && buf[0] != 0)
					lane._name = buf;
				else {
					sprintf_s(buf, "Unk(%u)", t._raw._id);
					lane._name = buf;
				}
			}
			laneIndex[key] = lanes.size();
			lanes.push_back(lane);
		}
	}

	std::vector<Entry> entries;
	entries.reserve(timed.size());
	for (const Timed& t : timed) {
		size_t li = laneIndex[laneKey(t._raw._id, t._raw._isPlayer)];
		lanes[li]._xs.push_back(t._seconds);
		lanes[li]._ys.push_back(static_cast<double>(li));

		Entry e;
		e._lane = li;
		e._skillId = t._raw._skillId;
		e._seconds = t._seconds;
		e._skillName = SkillName(t._raw._skillId);
		entries.push_back(e);
	}

	_lanes.swap(lanes);
	_entries.swap(entries);

	if (_selectedLane >= static_cast<int>(_lanes.size()))
		_selectedLane = -1;
}

bool SkillTimeline::PassesFilter(const Entry& e) const
{
	const Lane& lane = _lanes[e._lane];
	if (!_showBoss && !lane._isPlayer)
		return false;
	if (_selectedLane >= 0 && static_cast<int>(e._lane) != _selectedLane)
		return false;
	if (_search[0] != 0 && e._skillName.find(_search) == std::string::npos)
		return false;
	return true;
}

void SkillTimeline::FormatTime(double seconds, char* out, size_t len) const
{
	bool negative = seconds < 0;
	double abs = negative ? -seconds : seconds;
	int totalMs = static_cast<int>(abs * 1000.0 + 0.5);
	int minutes = totalMs / 60000;
	int secs = (totalMs / 1000) % 60;
	int ms = totalMs % 1000;
	sprintf_s(out, len, "%s%02d:%02d.%03d", negative ? "-" : "", minutes, secs, ms);
}

void SkillTimeline::UpdateTab()
{
	if (!ImGui::BeginTabItem(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE").data()))
		return;

	RebuildIfNeeded();

	DrawControls();

	if (_entries.empty()) {
		ImGui::TextDisabled("%s", LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_EMPTY").data());
	}
	else {
		DrawPlot();
		DrawList();
	}

	ImGui::EndTabItem();
}

void SkillTimeline::DrawControls()
{
	char label[256] = { 0 };

	// A boss lane hidden by the checkbox cannot stay selected.
	if (_selectedLane >= 0 && !_showBoss && !_lanes[_selectedLane]._isPlayer)
		_selectedLane = -1;

	const char* allText = LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_ALL").data();
	const char* preview = (_selectedLane < 0) ? allText : _lanes[_selectedLane]._name.c_str();

	sprintf_s(label, "%s###SkillTimelinePlayer", LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_PLAYER").data());
	ImGui::SetNextItemWidth(220.0f);
	if (ImGui::BeginCombo(label, preview, ImGuiComboFlags_HeightLarge)) {
		if (ImGui::Selectable(allText, _selectedLane < 0))
			_selectedLane = -1;
		for (size_t i = 0; i < _lanes.size(); i++) {
			if (!_showBoss && !_lanes[i]._isPlayer)
				continue;
			sprintf_s(label, "%s##lane%zu", _lanes[i]._name.c_str(), i);
			if (ImGui::Selectable(label, _selectedLane == static_cast<int>(i)))
				_selectedLane = static_cast<int>(i);
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine();
	ImGui::Checkbox(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_SHOW_BOSS").data(), &_showBoss);

	sprintf_s(label, "%s###SkillTimelineSearch", LANGMANAGER.GetText("STR_UTILLWINDOW_SEARCH").data());
	ImGui::SetNextItemWidth(220.0f);
	ImGui::InputText(label, _search, IM_ARRAYSIZE(_search));

	ImGui::SameLine();
	if (ImGui::Button(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_EXPORT").data()))
		ExportCsv();
	ImGui::SameLine();
	if (ImGui::Button(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_COPY").data()))
		CopyToClipboard();

	size_t shown = 0;
	for (const Entry& e : _entries)
		shown += PassesFilter(e) ? 1 : 0;
	ImGui::SameLine();
	ImGui::TextDisabled(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_COUNT").data(), static_cast<int>(shown));

	if (!_status.empty())
		ImGui::TextDisabled("%s", _status.c_str());
}

void SkillTimeline::DrawPlot()
{
	// Lanes that survive the boss/player filters, bottom to top.
	std::vector<size_t> visible;
	for (size_t i = 0; i < _lanes.size(); i++) {
		if (!_showBoss && !_lanes[i]._isPlayer)
			continue;
		if (_selectedLane >= 0 && static_cast<int>(i) != _selectedLane)
			continue;
		visible.push_back(i);
	}
	if (visible.empty())
		return;

	std::vector<double> tickValues(visible.size());
	std::vector<const char*> tickLabels(visible.size());
	std::vector<double> laneRow(_lanes.size(), -1.0);
	for (size_t row = 0; row < visible.size(); row++) {
		tickValues[row] = static_cast<double>(row);
		tickLabels[row] = _lanes[visible[row]]._name.c_str();
		laneRow[visible[row]] = static_cast<double>(row);
	}

	ImPlot::SetNextPlotLimitsY(-0.5, static_cast<double>(visible.size()) - 0.5, ImGuiCond_Always);
	ImPlot::SetNextPlotTicksY(tickValues.data(), static_cast<int>(tickValues.size()), tickLabels.data());

	if (!ImPlot::BeginPlot("##SkillTimelinePlot",
		LANGMANAGER.GetText("STR_PLOTWINDOW_TIME_SEC").data(), nullptr,
		ImVec2(-1, kPlotHeight),
		ImPlotFlags_AntiAliased | ImPlotFlags_NoMousePos,
		ImPlotAxisFlags_AutoFit,
		ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Invert))
		return;

	// Series per lane; the search filter is applied per point.
	std::vector<double> xs;
	std::vector<double> ys;
	char label[256] = { 0 };
	for (size_t row = 0; row < visible.size(); row++) {
		const Lane& lane = _lanes[visible[row]];
		xs.clear();
		ys.clear();
		for (const Entry& e : _entries) {
			if (e._lane != visible[row] || !PassesFilter(e))
				continue;
			xs.push_back(e._seconds);
			ys.push_back(static_cast<double>(row));
		}
		sprintf_s(label, "%s###lane%u_%d", lane._name.c_str(), lane._id, lane._isPlayer ? 1 : 0);
		ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 5.0f);
		ImPlot::PlotScatter(label, xs.data(), ys.data(), static_cast<int>(xs.size()));
	}

	if (ImPlot::IsPlotHovered()) {
		ImVec2 mouse = ImGui::GetMousePos();
		const Entry* best = nullptr;
		float bestDist = kHoverRadiusPx * kHoverRadiusPx;
		for (const Entry& e : _entries) {
			if (laneRow[e._lane] < 0 || !PassesFilter(e))
				continue;
			ImVec2 px = ImPlot::PlotToPixels(e._seconds, laneRow[e._lane]);
			float dx = px.x - mouse.x;
			float dy = px.y - mouse.y;
			float d = dx * dx + dy * dy;
			if (d < bestDist) {
				bestDist = d;
				best = &e;
			}
		}
		if (best != nullptr) {
			char timeText[32] = { 0 };
			FormatTime(best->_seconds, timeText, sizeof(timeText));
			ImGui::BeginTooltip();
			ImGui::Text("%s", _lanes[best->_lane]._name.c_str());
			ImGui::Text("%s", best->_skillName.c_str());
			ImGui::TextDisabled("%s", timeText);
			ImGui::EndTooltip();
		}
	}

	ImPlot::EndPlot();
}

void SkillTimeline::DrawList()
{
	std::vector<size_t> rows;
	rows.reserve(_entries.size());
	for (size_t i = 0; i < _entries.size(); i++) {
		if (PassesFilter(_entries[i]))
			rows.push_back(i);
	}

	ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
	if (!ImGui::BeginTable("SkillTimelineTable", 4, flags, ImVec2(0, 0)))
		return;

	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 50.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_TIME").data(), ImGuiTableColumnFlags_WidthFixed, 90.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_PLAYER").data(), ImGuiTableColumnFlags_WidthFixed, 140.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_SKILL").data(), ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableHeadersRow();

	char timeText[32] = { 0 };
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(rows.size()));
	while (clipper.Step()) {
		for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
			const Entry& e = _entries[rows[r]];
			FormatTime(e._seconds, timeText, sizeof(timeText));

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%d", r + 1);
			ImGui::TableNextColumn();
			ImGui::Text("%s", timeText);
			ImGui::TableNextColumn();
			ImGui::Text("%s", _lanes[e._lane]._name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%s", e._skillName.c_str());
		}
	}

	ImGui::EndTable();
}

bool SkillTimeline::ExportCsv()
{
	char path[MAX_PATH] = { 0 };
	std::error_code ec;
	std::filesystem::create_directories("Timeline", ec);

	SYSTEMTIME st;
	GetLocalTime(&st);
	sprintf_s(path, "Timeline\\SkillTimeline_%04d%02d%02d_%02d%02d%02d.csv",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

	std::ofstream file(path, std::ios::binary);
	if (!file) {
		_status = LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_EXPORT_FAILED").data();
		return false;
	}

	file << "\xEF\xBB\xBF";
	file << "time_sec,time,player,skill_id,skill\r\n";

	char timeText[32] = { 0 };
	char seconds[32] = { 0 };
	for (const Entry& e : _entries) {
		if (!PassesFilter(e))
			continue;
		FormatTime(e._seconds, timeText, sizeof(timeText));
		sprintf_s(seconds, "%.3f", e._seconds);
		file << seconds << ',' << timeText << ','
			<< CsvQuote(_lanes[e._lane]._name) << ',' << e._skillId << ','
			<< CsvQuote(e._skillName) << "\r\n";
	}
	file.close();

	char msg[MAX_PATH + 128] = { 0 };
	sprintf_s(msg, LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_EXPORTED").data(), path);
	_status = msg;
	return true;
}

void SkillTimeline::CopyToClipboard()
{
	std::string text;
	char timeText[32] = { 0 };
	for (const Entry& e : _entries) {
		if (!PassesFilter(e))
			continue;
		FormatTime(e._seconds, timeText, sizeof(timeText));
		text += "[";
		text += timeText;
		text += "] ";
		text += _lanes[e._lane]._name;
		text += " - ";
		text += e._skillName;
		text += "\n";
	}
	ImGui::SetClipboardText(text.c_str());
}
