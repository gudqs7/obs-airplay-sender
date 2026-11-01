#pragma once

#include <cstdint>
#include <limits>

inline int64_t ticks_to_usec(int64_t ticks, int32_t timebase_den)
{
	if (timebase_den <= 0)
		return 0;

	constexpr int64_t kMicroseconds = 1000000;
	const int64_t whole = ticks / timebase_den;
	const int64_t remainder = ticks % timebase_den;
	if (whole > std::numeric_limits<int64_t>::max() / kMicroseconds)
		return std::numeric_limits<int64_t>::max();
	if (whole < std::numeric_limits<int64_t>::min() / kMicroseconds)
		return std::numeric_limits<int64_t>::min();
	const int64_t scaled_whole = whole * kMicroseconds;
	const int64_t scaled_remainder = remainder * kMicroseconds / timebase_den;
	if (scaled_remainder > 0 && scaled_whole > std::numeric_limits<int64_t>::max() - scaled_remainder)
		return std::numeric_limits<int64_t>::max();
	if (scaled_remainder < 0 && scaled_whole < std::numeric_limits<int64_t>::min() - scaled_remainder)
		return std::numeric_limits<int64_t>::min();
	return scaled_whole + scaled_remainder;
}

// OBS exposes a system DTS. AirPlay's access-unit timestamp is a presentation
// time, so preserve the packet's PTS-DTS composition offset when one exists.
inline int64_t encoder_packet_system_pts_usec(int64_t sys_dts_usec, int64_t pts, int64_t dts, int32_t timebase_den)
{
	if (sys_dts_usec <= 0 || timebase_den <= 0)
		return sys_dts_usec;

	const int64_t composition_offset = ticks_to_usec(pts - dts, timebase_den);
	if (composition_offset > 0 && sys_dts_usec > std::numeric_limits<int64_t>::max() - composition_offset)
		return std::numeric_limits<int64_t>::max();
	if (composition_offset < 0 && sys_dts_usec < std::numeric_limits<int64_t>::min() - composition_offset)
		return std::numeric_limits<int64_t>::min();
	return sys_dts_usec + composition_offset;
}
