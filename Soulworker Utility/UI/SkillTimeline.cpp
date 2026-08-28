#include "pch.h"
#include "SkillTimeline.h"

#include ".\Combat Meter\Combat.h"
#include ".\Combat Meter\CombatMeter.h"
#include ".\Damage Meter\Damage Meter.h"
#include ".\Damage Meter\MySQLite.h"
#include ".\UI\Option.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
	constexpr float kRowPx = 22.0f;          // chart height per row
	constexpr float kChartPaddingPx = 70.0f; // axis, labels, margins
	constexpr float kChartMinPx = 220.0f;
	constexpr float kChartMaxShare = 0.6f;   // of the tab's remaining height
	constexpr double kBarHalfHeight = 0.38;  // in row units
	constexpr double kMinBarSeconds = 0.05;  // a cast with no damage is drawn this long
	constexpr float kBarMinPx = 4.0f;        // ... and never thinner than this
	constexpr float kBarLabelPadPx = 8.0f;

	// A hit can be logged a moment before the cast packet that produced it;
	// when no earlier cast of that skill exists, one this close still claims it.
	constexpr double kCastLeadMs = 250.0;

	// Raw entries copied out of the combat log while its lock is held; names
	// are resolved afterwards so no other lock is taken under the combat lock.
	struct RawCast {
		uint32_t _id;
		bool _isPlayer;
		uint32_t _skillId;
		double _elapsedMs;   // raid-timer time, 0 when unknown
		uint64_t _timestamp; // wall clock, ms
	};

	struct RawHit {
		uint32_t _id;
		bool _isPlayer;
		uint64_t _timestamp;
		double _damage;
		bool _crit;
		uint32_t _skillId;   // 0 when the log predates it
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

	uint64_t RowKey(size_t lane, uint32_t skillId)
	{
		return (static_cast<uint64_t>(lane) << 32) | skillId;
	}

	uint64_t LaneKey(uint32_t id, bool isPlayer)
	{
		return (static_cast<uint64_t>(isPlayer) << 32) | id;
	}

	// Skill ids are grouped by hundreds: the last two digits carry the level
	// (52001715 = Ghosty Hunt V) or the index of one of its sub-hits
	// (52001721 = Ghosty Hunt V - Ghosty Hunt I, 52001775 = Ghosty Hunt V [B]).
	// A hit whose exact id was never cast is tied to a cast of its family.
	uint32_t SkillFamily(uint32_t skillId)
	{
		return skillId / 100;
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
	std::vector<RawCast> casts;
	std::vector<RawHit> hits;

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
				switch (pLog->_type) {
				case CombatLogType::USED_SKILL:
				{
					RawCast raw;
					raw._id = pCombat->GetID();
					raw._isPlayer = isPlayer;
					raw._skillId = static_cast<uint32_t>(pLog->_val1);
					raw._elapsedMs = pLog->_val2;
					raw._timestamp = log->first;
					casts.push_back(raw);
					break;
				}
				case CombatLogType::GIVE_DAMAGE_NORMAL:
				case CombatLogType::GIVE_DAMAGE_CRIT:
				case CombatLogType::GIVE_DAMAGE_MISS:
				{
					RawHit hit;
					hit._id = pCombat->GetID();
					hit._isPlayer = isPlayer;
					hit._timestamp = log->first;
					hit._damage = pLog->_val1;
					hit._crit = pLog->_type == CombatLogType::GIVE_DAMAGE_CRIT;
					hit._skillId = static_cast<uint32_t>(pLog->_val3);
					hits.push_back(hit);
					break;
				}
				default:
					break;
				}
			}
		}
		COMBATMETER.FreeLock();
	}

	// Time base. Casts stamped with the raid timer are exact; anything else
	// (older histories, casts before the timer started, and every damage hit)
	// is placed by wall clock relative to the same origin, which puts pre-pull
	// casts at negative times.
	double originTs = 0;
	bool haveOrigin = false;
	uint64_t minTs = UINT64_MAX;
	for (const RawCast& raw : casts) {
		if (!haveOrigin && raw._elapsedMs > 0) {
			originTs = static_cast<double>(raw._timestamp) - raw._elapsedMs;
			haveOrigin = true;
		}
		minTs = (raw._timestamp < minTs) ? raw._timestamp : minTs;
	}
	if (!haveOrigin && !casts.empty())
		originTs = static_cast<double>(minTs);

	struct Timed {
		RawCast _raw;
		double _seconds;
	};
	std::vector<Timed> timed;
	timed.reserve(casts.size());
	for (const RawCast& raw : casts) {
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

	for (int pass = 0; pass < 2; pass++) {
		bool wantPlayer = (pass == 0);
		for (const Timed& t : timed) {
			if (t._raw._isPlayer != wantPlayer)
				continue;
			uint64_t key = LaneKey(t._raw._id, t._raw._isPlayer);
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
	std::vector<int> lastOfLane(lanes.size(), -1);
	for (const Timed& t : timed) {
		size_t li = laneIndex[LaneKey(t._raw._id, t._raw._isPlayer)];

		Entry e;
		e._lane = li;
		e._skillId = t._raw._skillId;
		e._seconds = t._seconds;
		e._timestamp = t._raw._timestamp;
		e._nextTimestamp = 0;
		e._lastOwnHit = -1;
		e._skillName = SkillName(t._raw._skillId);

		// A cast closes the previous cast's loose-hit window on the same lane.
		if (lastOfLane[li] >= 0)
			entries[lastOfLane[li]]._nextTimestamp = t._raw._timestamp;
		lastOfLane[li] = static_cast<int>(entries.size());

		entries.push_back(e);
	}

	// Damage. A hit goes to the latest cast of its skill on the same lane -
	// by exact id first, then by skill family - compared by wall clock, which
	// both sides share; anything that cannot be tied to a cast that way is
	// "loose" and falls to whichever bar covers it. Hits from entities that
	// never cast anything have no lane and are dropped.
	std::unordered_map<uint64_t, std::vector<size_t>> castsOf;      // (lane, skill id)
	std::unordered_map<uint64_t, std::vector<size_t>> familyCasts;  // (lane, skill family)
	for (size_t i = 0; i < entries.size(); i++) {
		castsOf[RowKey(entries[i]._lane, entries[i]._skillId)].push_back(i);
		familyCasts[RowKey(entries[i]._lane, SkillFamily(entries[i]._skillId))].push_back(i);
	}
	auto byTime = [&entries](size_t a, size_t b) {
		return entries[a]._timestamp < entries[b]._timestamp;
	};
	for (auto& kv : castsOf)
		std::sort(kv.second.begin(), kv.second.end(), byTime);
	for (auto& kv : familyCasts)
		std::sort(kv.second.begin(), kv.second.end(), byTime);

	// The latest of these casts at or before the hit, or one following it
	// within kCastLeadMs when there is none before; -1 when neither.
	auto claim = [&entries](const std::vector<size_t>& casts, uint64_t hitTs) -> int {
		auto after = std::upper_bound(casts.begin(), casts.end(), hitTs,
			[&entries](uint64_t ts, size_t idx) { return ts < entries[idx]._timestamp; });
		if (after != casts.begin())
			return static_cast<int>(*(after - 1));
		if (after != casts.end() &&
			static_cast<double>(entries[*after]._timestamp - hitTs) <= kCastLeadMs)
			return static_cast<int>(*after);
		return -1;
	};

	std::stable_sort(hits.begin(), hits.end(), [](const RawHit& a, const RawHit& b) {
		return a._timestamp < b._timestamp;
	});
	for (Lane& lane : lanes) {
		lane._loosePrefix.push_back(0);
		lane._looseHitPrefix.push_back(0);
		lane._looseCritPrefix.push_back(0);
	}
	for (const RawHit& hit : hits) {
		auto found = laneIndex.find(LaneKey(hit._id, hit._isPlayer));
		if (found == laneIndex.end())
			continue;
		size_t li = found->second;

		int owner = -1;
		if (hit._skillId != 0) {
			auto exact = castsOf.find(RowKey(li, hit._skillId));
			if (exact != castsOf.end())
				owner = claim(exact->second, hit._timestamp);
			if (owner < 0) {
				auto family = familyCasts.find(RowKey(li, SkillFamily(hit._skillId)));
				if (family != familyCasts.end())
					owner = claim(family->second, hit._timestamp);
			}
		}

		if (owner >= 0) {
			Entry& e = entries[owner];
			e._own._damage += hit._damage;
			e._own._hits += 1;
			e._own._crits += hit._crit ? 1 : 0;
			double offset = (static_cast<double>(hit._timestamp) - static_cast<double>(e._timestamp)) / 1000.0;
			if (offset < 0)
				offset = 0;
			if (offset > e._lastOwnHit)
				e._lastOwnHit = offset;
		}
		else {
			Lane& lane = lanes[li];
			lane._looseTimes.push_back(static_cast<double>(hit._timestamp));
			lane._loosePrefix.push_back(lane._loosePrefix.back() + hit._damage);
			lane._looseHitPrefix.push_back(lane._looseHitPrefix.back() + 1);
			lane._looseCritPrefix.push_back(lane._looseCritPrefix.back() + (hit._crit ? 1 : 0));
		}
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

// Loose hits fall to the lane's latest cast before them: [a, b) indexes the
// lane's loose arrays for this cast's window, which runs from the cast to
// the lane's next cast, or open-ended for its last one.
void SkillTimeline::LooseRange(const Entry& e, size_t& a, size_t& b) const
{
	const Lane& lane = _lanes[e._lane];
	auto lo = std::lower_bound(lane._looseTimes.begin(), lane._looseTimes.end(), static_cast<double>(e._timestamp));
	auto hi = lane._looseTimes.end();
	if (e._nextTimestamp != 0)
		hi = std::lower_bound(lo, lane._looseTimes.end(), static_cast<double>(e._nextTimestamp));
	a = static_cast<size_t>(lo - lane._looseTimes.begin());
	b = static_cast<size_t>(hi - lane._looseTimes.begin());
}

// The drawn end: the last hit credited to the cast, own or loose, or a
// sliver when it dealt none.
double SkillTimeline::EndOf(const Entry& e) const
{
	double end = e._seconds + kMinBarSeconds;
	if (e._lastOwnHit >= 0 && e._seconds + e._lastOwnHit > end)
		end = e._seconds + e._lastOwnHit;

	size_t a, b;
	LooseRange(e, a, b);
	if (b > a) {
		double lastLoose = e._seconds +
			(_lanes[e._lane]._looseTimes[b - 1] - static_cast<double>(e._timestamp)) / 1000.0;
		if (lastLoose > end)
			end = lastLoose;
	}
	return end;
}

// The cast's own hits plus the loose hits in its window.
SkillTimeline::DamageWindow SkillTimeline::DamageIn(const Entry& e) const
{
	DamageWindow w = e._own;
	size_t a, b;
	LooseRange(e, a, b);
	if (b > a) {
		const Lane& lane = _lanes[e._lane];
		w._damage += lane._loosePrefix[b] - lane._loosePrefix[a];
		w._hits += lane._looseHitPrefix[b] - lane._looseHitPrefix[a];
		w._crits += lane._looseCritPrefix[b] - lane._looseCritPrefix[a];
	}
	return w;
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

// Same presentation as the damage table: the 1K / 10K / 1M option scales the
// number and appends its unit, everything else gets thousands separators.
void SkillTimeline::FormatDamage(double damage, char* out, size_t len) const
{
	double value = damage;
	const char* unit = "";
	if (UIOPTION.is1K()) {
		value /= 1000.0;
		unit = LANGMANAGER.GetText("STR_DISPLAY_UNIT_1K").data();
	}
	else if (UIOPTION.is1M()) {
		value /= 1000000.0;
		unit = LANGMANAGER.GetText("STR_DISPLAY_UNIT_1M").data();
	}
	else if (UIOPTION.is10K()) {
		value /= 10000.0;
		unit = LANGMANAGER.GetText("STR_DISPLAY_UNIT_10K").data();
	}

	char comma[128] = { 0 };
	if (UIOPTION.is1M()) {
		TextCommmaIncludeDecimal(value, sizeof(comma), comma);
	}
	else {
		char plain[64] = { 0 };
		sprintf_s(plain, "%.0f", value);
		TextCommma(plain, comma);
	}
	sprintf_s(out, len, "%s%s", comma, unit);
}

void SkillTimeline::FormatDamageShort(double damage, char* out, size_t len) const
{
	if (UIOPTION.is1K())
		sprintf_s(out, len, "%.0f%s", damage / 1000.0, LANGMANAGER.GetText("STR_DISPLAY_UNIT_1K").data());
	else if (UIOPTION.is1M())
		sprintf_s(out, len, "%.1f%s", damage / 1000000.0, LANGMANAGER.GetText("STR_DISPLAY_UNIT_1M").data());
	else if (UIOPTION.is10K())
		sprintf_s(out, len, "%.0f%s", damage / 10000.0, LANGMANAGER.GetText("STR_DISPLAY_UNIT_10K").data());
	else if (damage >= 1000000000.0)
		sprintf_s(out, len, "%.2fB", damage / 1000000000.0);
	else if (damage >= 1000000.0)
		sprintf_s(out, len, "%.1fM", damage / 1000000.0);
	else if (damage >= 1000.0)
		sprintf_s(out, len, "%.0fK", damage / 1000.0);
	else
		sprintf_s(out, len, "%.0f", damage);
}

void SkillTimeline::BuildRows(std::vector<Row>& rows, std::vector<int>& rowOfEntry) const
{
	rows.clear();
	rowOfEntry.assign(_entries.size(), -1);

	// First pass: rows in first-use order, keyed by (lane, skill).
	std::unordered_map<uint64_t, int> index;
	for (size_t i = 0; i < _entries.size(); i++) {
		const Entry& e = _entries[i];
		if (!PassesFilter(e))
			continue;
		uint64_t key = RowKey(e._lane, e._skillId);
		auto found = index.find(key);
		if (found == index.end()) {
			Row row;
			row._lane = e._lane;
			row._skillId = e._skillId;
			if (_selectedLane < 0)
				row._label = _lanes[e._lane]._name + " | " + e._skillName;
			else
				row._label = e._skillName;
			index[key] = static_cast<int>(rows.size());
			rows.push_back(row);
		}
	}

	// Group by lane, keeping first-use order inside each lane.
	std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
		return a._lane < b._lane;
	});
	index.clear();
	for (size_t r = 0; r < rows.size(); r++)
		index[RowKey(rows[r]._lane, rows[r]._skillId)] = static_cast<int>(r);

	for (size_t i = 0; i < _entries.size(); i++) {
		const Entry& e = _entries[i];
		if (!PassesFilter(e))
			continue;
		rowOfEntry[i] = index[RowKey(e._lane, e._skillId)];
	}
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
		DrawGantt();
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
	ImGui::SameLine();
	if (ImGui::Button(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_FIT").data()))
		_refit = true;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_ZOOM_HINT").data());

	size_t shown = 0;
	double shownDamage = 0;
	for (const Entry& e : _entries) {
		if (!PassesFilter(e))
			continue;
		shown++;
		shownDamage += DamageIn(e)._damage;
	}
	char damageText[128] = { 0 };
	FormatDamage(shownDamage, damageText, sizeof(damageText));
	ImGui::SameLine();
	ImGui::TextDisabled(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_COUNT").data(), static_cast<int>(shown));
	ImGui::SameLine();
	ImGui::TextDisabled("| %s %s", LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_DAMAGE").data(), damageText);

	if (!_status.empty())
		ImGui::TextDisabled("%s", _status.c_str());
}

void SkillTimeline::DrawGantt()
{
	std::vector<Row> rows;
	std::vector<int> rowOfEntry;
	BuildRows(rows, rowOfEntry);
	if (rows.empty())
		return;

	std::vector<double> tickValues(rows.size());
	std::vector<const char*> tickLabels(rows.size());
	for (size_t r = 0; r < rows.size(); r++) {
		tickValues[r] = static_cast<double>(r);
		tickLabels[r] = rows[r]._label.c_str();
	}

	// Extents feed an invisible series so the x axis keeps auto-fitting.
	double minX = DBL_MAX;
	double maxX = -DBL_MAX;
	for (size_t i = 0; i < _entries.size(); i++) {
		if (rowOfEntry[i] < 0)
			continue;
		minX = (_entries[i]._seconds < minX) ? _entries[i]._seconds : minX;
		double end = EndOf(_entries[i]);
		maxX = (end > maxX) ? end : maxX;
	}
	if (minX > 0)
		minX = 0;
	double fitX[2] = { minX, maxX };
	double fitY[2] = { 0.0, static_cast<double>(rows.size()) - 1.0 };

	// The chart grows with its rows and scrolls once it would crowd the list.
	float wanted = static_cast<float>(rows.size()) * kRowPx + kChartPaddingPx;
	if (wanted < kChartMinPx)
		wanted = kChartMinPx;
	float region = ImGui::GetContentRegionAvail().y * kChartMaxShare;
	if (region < kChartMinPx)
		region = kChartMinPx;
	float childHeight = (wanted < region) ? wanted : region;

	// Ctrl + wheel zooms the time axis. A plain wheel keeps scrolling the
	// rows - ImGui has already done that by now and skips scrolling while
	// Ctrl is held - so only the zoom needs gating here.
	ImGuiIO& io = ImGui::GetIO();
	float wheel = io.MouseWheel;
	if (!io.KeyCtrl)
		io.MouseWheel = 0.0f;

	ImGui::BeginChild("SkillTimelineGantt", ImVec2(0, childHeight), false);
	{
		// Follow the data until the user zooms or pans away from its extents.
		if (_refit || _following) {
			ImPlot::SetNextPlotLimitsX(minX, maxX, ImGuiCond_Always);
			_refit = false;
		}
		ImPlot::SetNextPlotLimitsY(-0.5, static_cast<double>(rows.size()) - 0.5, ImGuiCond_Always);
		ImPlot::SetNextPlotTicksY(tickValues.data(), static_cast<int>(tickValues.size()), tickLabels.data());

		if (ImPlot::BeginPlot("##SkillTimelineGantt",
			LANGMANAGER.GetText("STR_PLOTWINDOW_TIME_SEC").data(), nullptr,
			ImVec2(-1, wanted),
			ImPlotFlags_NoLegend | ImPlotFlags_NoMousePos | ImPlotFlags_NoChild | ImPlotFlags_NoMenus,
			ImPlotAxisFlags_None,
			ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_Invert))
		{
			// ImPlot has applied this frame's zoom/pan by now; a double-click
			// fit lands exactly on the extents too (FitPadding is zero), so
			// the same test also picks following back up after one.
			ImPlotLimits limits = ImPlot::GetPlotLimits();
			double tol = (maxX - minX) * 1e-6 + 1e-9;
			_following = fabs(limits.X.Min - minX) <= tol && fabs(limits.X.Max - maxX) <= tol;

			ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 0.0f, ImVec4(0, 0, 0, 0), 0.0f, ImVec4(0, 0, 0, 0));
			ImPlot::PlotScatter("##fit", fitX, fitY, 2);

			ImDrawList* draw = ImPlot::GetPlotDrawList();
			ImVec2 mouse = ImGui::GetMousePos();
			bool hovered = ImPlot::IsPlotHovered();
			int hoveredEntry = -1;
			char barText[32] = { 0 };

			ImPlot::PushPlotClipRect();
			for (size_t i = 0; i < _entries.size(); i++) {
				int row = rowOfEntry[i];
				if (row < 0)
					continue;
				const Entry& e = _entries[i];

				ImVec2 a = ImPlot::PlotToPixels(e._seconds, static_cast<double>(row) - kBarHalfHeight);
				ImVec2 b = ImPlot::PlotToPixels(EndOf(e), static_cast<double>(row) + kBarHalfHeight);
				ImVec2 pmin(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y);
				ImVec2 pmax(a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y);
				if (pmax.x - pmin.x < kBarMinPx)
					pmax.x = pmin.x + kBarMinPx;

				ImVec4 color = ImPlot::GetColormapColor(row);
				bool isHovered = hovered &&
					mouse.x >= pmin.x && mouse.x <= pmax.x &&
					mouse.y >= pmin.y && mouse.y <= pmax.y;
				if (isHovered)
					hoveredEntry = static_cast<int>(i);

				ImU32 fill = ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, isHovered ? 1.0f : 0.8f));
				ImU32 edge = ImGui::GetColorU32(ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, 1.0f));
				draw->AddRectFilled(pmin, pmax, fill, 2.0f);
				draw->AddRect(pmin, pmax, edge, 2.0f);

				// Damage label inside the bar when there is room for it.
				DamageWindow w = DamageIn(e);
				if (w._damage > 0) {
					FormatDamageShort(w._damage, barText, sizeof(barText));
					ImVec2 textSize = ImGui::CalcTextSize(barText);
					if (textSize.x + kBarLabelPadPx <= pmax.x - pmin.x && textSize.y <= pmax.y - pmin.y) {
						float luma = 0.299f * color.x + 0.587f * color.y + 0.114f * color.z;
						ImU32 textColor = luma > 0.6f ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
						ImVec2 textPos(pmin.x + (pmax.x - pmin.x - textSize.x) * 0.5f,
							pmin.y + (pmax.y - pmin.y - textSize.y) * 0.5f);
						draw->AddText(textPos, textColor, barText);
					}
				}
			}
			ImPlot::PopPlotClipRect();

			if (hoveredEntry >= 0) {
				const Entry& e = _entries[hoveredEntry];
				DamageWindow w = DamageIn(e);
				char startText[32] = { 0 };
				char endText[32] = { 0 };
				char damageText[128] = { 0 };
				FormatTime(e._seconds, startText, sizeof(startText));
				FormatTime(EndOf(e), endText, sizeof(endText));
				FormatDamage(w._damage, damageText, sizeof(damageText));
				ImGui::BeginTooltip();
				ImGui::Text("%s", _lanes[e._lane]._name.c_str());
				ImGui::Text("%s", e._skillName.c_str());
				ImGui::TextDisabled("%s - %s (%.2f s)", startText, endText, EndOf(e) - e._seconds);
				ImGui::Text("%s %s", LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_DAMAGE").data(), damageText);
				ImGui::TextDisabled(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_HITS").data(), w._hits, w._crits);
				ImGui::EndTooltip();
			}

			ImPlot::EndPlot();
		}
	}
	ImGui::EndChild();
	io.MouseWheel = wheel;
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
	if (!ImGui::BeginTable("SkillTimelineTable", 7, flags, ImVec2(0, 0)))
		return;

	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 50.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_TIME").data(), ImGuiTableColumnFlags_WidthFixed, 90.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_DURATION").data(), ImGuiTableColumnFlags_WidthFixed, 80.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_PLAYER").data(), ImGuiTableColumnFlags_WidthFixed, 140.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_SKILL").data(), ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_PLOTWINDOW_SKILLTIMELINE_DAMAGE").data(), ImGuiTableColumnFlags_WidthFixed, 120.0f);
	ImGui::TableSetupColumn(LANGMANAGER.GetText("STR_TABLE_TOTAL_HIT").data(), ImGuiTableColumnFlags_WidthFixed, 70.0f);
	ImGui::TableHeadersRow();

	char timeText[32] = { 0 };
	char damageText[128] = { 0 };
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(rows.size()));
	while (clipper.Step()) {
		for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
			const Entry& e = _entries[rows[r]];
			DamageWindow w = DamageIn(e);
			FormatTime(e._seconds, timeText, sizeof(timeText));
			FormatDamage(w._damage, damageText, sizeof(damageText));

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%d", r + 1);
			ImGui::TableNextColumn();
			ImGui::Text("%s", timeText);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f s", EndOf(e) - e._seconds);
			ImGui::TableNextColumn();
			ImGui::Text("%s", _lanes[e._lane]._name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%s", e._skillName.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%s", damageText);
			ImGui::TableNextColumn();
			ImGui::Text("%d (%d)", w._hits, w._crits);
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
	file << "start_sec,end_sec,duration_sec,time,player,skill_id,skill,damage,hits,crits\r\n";

	char timeText[32] = { 0 };
	char numbers[96] = { 0 };
	char tail[96] = { 0 };
	for (const Entry& e : _entries) {
		if (!PassesFilter(e))
			continue;
		double end = EndOf(e);
		DamageWindow w = DamageIn(e);
		FormatTime(e._seconds, timeText, sizeof(timeText));
		sprintf_s(numbers, "%.3f,%.3f,%.3f", e._seconds, end, end - e._seconds);
		sprintf_s(tail, "%.0f,%d,%d", w._damage, w._hits, w._crits);
		file << numbers << ',' << timeText << ','
			<< CsvQuote(_lanes[e._lane]._name) << ',' << e._skillId << ','
			<< CsvQuote(e._skillName) << ',' << tail << "\r\n";
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
	char duration[32] = { 0 };
	char damageText[128] = { 0 };
	for (const Entry& e : _entries) {
		if (!PassesFilter(e))
			continue;
		DamageWindow w = DamageIn(e);
		FormatTime(e._seconds, timeText, sizeof(timeText));
		sprintf_s(duration, "%.2fs", EndOf(e) - e._seconds);
		FormatDamage(w._damage, damageText, sizeof(damageText));
		text += "[";
		text += timeText;
		text += "] ";
		text += _lanes[e._lane]._name;
		text += " - ";
		text += e._skillName;
		text += " (";
		text += duration;
		text += ") ";
		text += damageText;
		text += "\n";
	}
	ImGui::SetClipboardText(text.c_str());
}
