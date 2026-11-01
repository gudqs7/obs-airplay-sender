#pragma once

inline bool resolve_auto_cast_on_obs_start(bool has_user_value, bool configured_value)
{
	return has_user_value ? configured_value : true;
}

enum class InitialScanAction {
	WaitForAutoCast,
	Scan,
	ShowStopBeforeScan,
};

inline InitialScanAction decide_initial_scan_action(bool cast_stopped, bool auto_cast_waiting)
{
	if (!cast_stopped)
		return InitialScanAction::ShowStopBeforeScan;
	if (auto_cast_waiting)
		return InitialScanAction::WaitForAutoCast;
	return InitialScanAction::Scan;
}

inline int normalize_playout_latency_ms(int value)
{
	if (value <= 0)
		return 0;
	if (value < 80)
		return 80;
	return value <= 500 ? value : 0;
}
