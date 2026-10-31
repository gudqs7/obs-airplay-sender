#pragma once

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
