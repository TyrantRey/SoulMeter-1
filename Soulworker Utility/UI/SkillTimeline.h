#pragma once
#include "pch.h"

// Skill Timeline tab for the Graph window, drawn as a Gantt chart.
//
// Every USED_SKILL entry from the combat log becomes a bar: one row per
// (player, skill), x = seconds on the raid timer. A bar runs from the cast
// to the last hit credited to it, so the chart reads as what was pressed,
// when, and for how long it kept dealing damage; a cast with no damage is a
// sliver. The damage comes from the GIVE_DAMAGE entries of the same log:
//
//  - A hit that carries a skill id belongs to the latest cast of that skill
//    on the same lane, wherever on the timeline it lands, so a
//    damage-over-time skill keeps its ticks even after the next skill is
//    pressed. Many skills deal their damage under sub-skill ids that are
//    never cast (52001715 Ghosty Hunt V hits as 52001721..24, 52001725 [A],
//    52001775 [B]); those fall back to the latest cast of the same skill
//    family, ids grouped by hundreds.
//  - A hit without a usable skill id (histories saved before it was
//    recorded, or a family that never appears as a cast on that lane) is
//    credited to the lane's latest cast before it.
//
// The time axis zooms with Ctrl + mouse wheel and pans by dragging (a plain
// wheel scrolls the rows); the chart follows the data until the user does
// either, and the Fit button or a double-click brings the whole run back.
// Below the chart is a filterable chronological list; the same rows can be
// exported to CSV or copied to the clipboard.
//
// Data source: USED_SKILL entries carry the skill id in _val1 and, since this
// feature, the raid-timer elapsed time in milliseconds in _val2. Entries
// recorded without it (older histories, casts before the timer started) are
// placed by wall clock relative to the same origin, so the tab works for live
// runs and for every saved history. GIVE_DAMAGE entries carry the damage in
// _val1 and the skill id in _val3; hits are matched to casts by wall clock,
// which both sides share, so a raid timer paused mid-run cannot skew the
// attribution.

#define SKILLTIMELINE SkillTimeline::getInstance()

class SkillTimeline : public Singleton<SkillTimeline> {
private:
	struct DamageWindow {
		double _damage = 0;
		int _hits = 0;
		int _crits = 0;
	};

	struct Lane {
		uint32_t _id;
		bool _isPlayer;
		std::string _name;

		// Hits that could not be tied to a cast by skill id, sorted by wall
		// clock (ms) with prefix sums, so any window is a pair of binary
		// searches.
		std::vector<double> _looseTimes;
		std::vector<double> _loosePrefix;   // size + 1
		std::vector<int> _looseHitPrefix;   // size + 1
		std::vector<int> _looseCritPrefix;  // size + 1
	};

	struct Entry {
		size_t _lane;
		uint32_t _skillId;
		double _seconds;      // cast time on the chart
		uint64_t _timestamp;  // cast time, wall clock ms
		uint64_t _nextTimestamp;  // same lane's next cast, wall clock ms, 0 when none yet
		DamageWindow _own;    // hits tied to this cast by skill id
		double _lastOwnHit;   // seconds after the cast of the last such hit, or -1
		std::string _skillName;
	};

	struct Row {
		size_t _lane;
		uint32_t _skillId;
		std::string _label;
	};

	std::vector<Lane> _lanes;
	std::vector<Entry> _entries;          // chronological
	std::unordered_map<uint32_t, std::string> _skillNameCache;

	// Snapshot identity, so a changed log is rebuilt and an unchanged one is not.
	const void* _snapshotSource = nullptr;
	size_t _snapshotCount = 0;

	bool _showBoss = false;
	int _selectedLane = -1;               // -1 = all
	char _search[128] = { 0 };
	std::string _status;

	// Time-axis view: the chart shows the whole run and keeps following it
	// until the user zooms or pans; Fit (or a double-click) returns to that.
	bool _refit = true;                   // apply the data extents this frame
	bool _following = true;               // view still equals the data extents

	void RebuildIfNeeded();
	void Rebuild();
	const char* SkillName(uint32_t skillId);

	bool PassesFilter(const Entry& e) const;
	void LooseRange(const Entry& e, size_t& a, size_t& b) const;  // loose hits in the cast's window
	double EndOf(const Entry& e) const;                            // last hit credited to the cast
	DamageWindow DamageIn(const Entry& e) const;

	void FormatTime(double seconds, char* out, size_t len) const;
	void FormatDamage(double damage, char* out, size_t len) const;       // table style: commas + unit option
	void FormatDamageShort(double damage, char* out, size_t len) const;  // bar label: compact

	// Rows for the current filter, grouped by lane in first-use order;
	// rowOfEntry maps every entry index to its row (or -1 when filtered out).
	void BuildRows(std::vector<Row>& rows, std::vector<int>& rowOfEntry) const;

	void DrawControls();
	void DrawGantt();
	void DrawList();

	bool ExportCsv();
	void CopyToClipboard();

public:
	SkillTimeline() {}
	~SkillTimeline() {}

	// Draws the tab item; call between BeginTabBar/EndTabBar.
	void UpdateTab();

	// Drops the cached snapshot (e.g. when the combat log is cleared).
	void Clear();
};
