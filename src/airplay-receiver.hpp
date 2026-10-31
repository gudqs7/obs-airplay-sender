#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "airplay-bridge.hpp"

inline bool is_douyin_receiver_name(std::string_view name)
{
	constexpr std::string_view prefix = "抖音直播伴侣[";
	return name.rfind(prefix, 0) == 0 && name.back() == ']';
}

inline const AirPlayDeviceInfo *find_douyin_receiver(const std::vector<AirPlayDeviceInfo> &devices,
						     std::string_view preferred)
{
	const AirPlayDeviceInfo *best = nullptr;
	for (const AirPlayDeviceInfo &device : devices) {
		if (!is_douyin_receiver_name(device.name))
			continue;
		if (!preferred.empty() && device.name == preferred)
			return &device;
		if (!best || device.name < best->name)
			best = &device;
	}
	return best;
}
