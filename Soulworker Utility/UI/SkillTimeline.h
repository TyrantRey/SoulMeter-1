#pragma once
#include "pch.h"

// Skill Timeline tab for the Graph window.
//
// Draws every USED_SKILL entry from the combat log as a marker on a per-entity
// lane (x = seconds on the raid timer), with a hover tooltip, a filterable
// chronological list under the plot, and CSV export / clipboard copy.
//
// The data source is the existing combat log: USED_SKILL entries carry the
// skill id in _val1 and, since this feature, the raid-timer elapsed time in
// milliseconds in _val2. Entries recorded before that (older histories) or
// before the timer started fall back to wall-clock offsets, so the tab works
// for live runs and for every saved history.

#define SKILLTIMELINE SkillTimeline::getInstance()

class Combat;

class SkillTimeline : public Singleton<SkillTimeline> {
private:
	struct Lane {
		uint32_t _id;
		bool _isPlayer;
		std::string _name;
		std::vector<double> _xs;   // seconds
		std::vector<double> _ys;   // lane index, repeated
	};

	struct Entry {
		size_t _lane;
		uint32_t _skillId;
		double _seconds;
		std::string _skillName;
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

	void RebuildIfNeeded();
	void Rebuild();
	const char* SkillName(uint32_t skillId);

	bool PassesFilter(const Entry& e) const;
	void FormatTime(double seconds, char* out, size_t len) const;

	void DrawControls();
	void DrawPlot();
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
