#pragma once
#include "pch.h"

// Skill Timeline tab for the Graph window, drawn as a Gantt chart.
//
// Every USED_SKILL entry from the combat log becomes a bar: one row per
// (player, skill), x = seconds on the raid timer. A bar runs from the cast
// until that player's next cast, capped by an adjustable maximum, so the
// chart reads as the rotation - what was pressed, in what order, with the
// gaps. Below the chart is a filterable chronological list; the same rows
// can be exported to CSV or copied to the clipboard.
//
// Data source: USED_SKILL entries carry the skill id in _val1 and, since this
// feature, the raid-timer elapsed time in milliseconds in _val2. Entries
// recorded without it (older histories, casts before the timer started) are
// placed by wall clock relative to the same origin, so the tab works for live
// runs and for every saved history.

#define SKILLTIMELINE SkillTimeline::getInstance()

class SkillTimeline : public Singleton<SkillTimeline> {
private:
	struct Lane {
		uint32_t _id;
		bool _isPlayer;
		std::string _name;
	};

	struct Entry {
		size_t _lane;
		uint32_t _skillId;
		double _seconds;      // cast time
		double _next;         // same lane's next cast, or -1 when none yet
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
	float _maxBarSeconds = 3.0f;
	char _search[128] = { 0 };
	std::string _status;

	void RebuildIfNeeded();
	void Rebuild();
	const char* SkillName(uint32_t skillId);

	bool PassesFilter(const Entry& e) const;
	double EndOf(const Entry& e) const;
	void FormatTime(double seconds, char* out, size_t len) const;

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
