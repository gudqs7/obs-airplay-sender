#pragma once

inline bool resolve_auto_cast_on_obs_start(bool has_user_value, bool configured_value)
{
	return has_user_value ? configured_value : true;
}
