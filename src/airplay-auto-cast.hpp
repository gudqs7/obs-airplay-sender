#pragma once

#include <algorithm>
#include <chrono>

#include "airplay-cast-state.hpp"

enum class AutoCastMonitorAction {
	None,
	Wait,
	Scan,
};

inline AutoCastMonitorAction decide_auto_cast_monitor_action(bool armed, AirPlayCastState state)
{
	if (!armed)
		return AutoCastMonitorAction::None;
	if (state == AirPlayCastState::Stopped)
		return AutoCastMonitorAction::Scan;
	return AutoCastMonitorAction::Wait;
}

inline std::chrono::seconds auto_cast_retry_delay(int failed_attempts, bool stable_session)
{
	if (stable_session)
		return std::chrono::seconds(3);
	switch (std::max(failed_attempts, 1)) {
	case 1:
		return std::chrono::seconds(3);
	case 2:
		return std::chrono::seconds(10);
	case 3:
		return std::chrono::seconds(30);
	default:
		return std::chrono::seconds(60);
	}
}
