#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "airplay-auto-cast.hpp"
#include "airplay-receiver.hpp"
#include "airplay-settings.hpp"

namespace {

void expect(bool condition, const char *message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	std::exit(1);
}

AirPlayDeviceInfo device(std::string name, std::string ip, uint16_t port)
{
	AirPlayDeviceInfo result;
	result.name = std::move(name);
	result.ip = std::move(ip);
	result.port = port;
	return result;
}

} // namespace

int main()
{
	expect(is_douyin_receiver_name("抖音直播伴侣[721246]"), "standard Douyin receiver name should match");
	expect(!is_douyin_receiver_name("抖音直播伴侣"), "bare Douyin display name should not match");
	expect(!is_douyin_receiver_name("客厅电视[721246]"), "non-Douyin receiver should not match");
	expect(!is_douyin_receiver_name("抖音直播伴侣[721246] extra"), "trailing text should not match");

	std::vector<AirPlayDeviceInfo> devices = {
		device("客厅电视", "192.168.1.10", 7000),
		device("抖音直播伴侣[900002]", "192.168.1.12", 7000),
		device("抖音直播伴侣[100001]", "192.168.1.11", 7000),
	};
	const AirPlayDeviceInfo *best = find_douyin_receiver(devices, {});
	expect(best != nullptr && best->name == "抖音直播伴侣[100001]",
	       "lowest-name Douyin receiver should be selected deterministically");

	best = find_douyin_receiver(devices, "抖音直播伴侣[900002]");
	expect(best != nullptr && best->ip == "192.168.1.12", "saved preferred Douyin receiver should win");

	std::vector<AirPlayDeviceInfo> no_douyin = {
		device("客厅电视", "192.168.1.10", 7000),
	};
	expect(find_douyin_receiver(no_douyin, {}) == nullptr, "no Douyin receiver should produce no match");

	expect(resolve_auto_cast_on_obs_start(false, false),
	       "unconfigured auto-cast setting should default to enabled");
	expect(!resolve_auto_cast_on_obs_start(true, false),
	       "an explicitly disabled auto-cast setting must remain disabled");
	expect(resolve_auto_cast_on_obs_start(true, true),
	       "an explicitly enabled auto-cast setting must remain enabled");

	expect(decide_auto_cast_monitor_action(false, AirPlayCastState::Stopped) ==
		       AutoCastMonitorAction::None,
	       "manual stop must disable automatic reconnect");
	expect(decide_auto_cast_monitor_action(true, AirPlayCastState::Running) ==
		       AutoCastMonitorAction::Wait,
	       "armed monitor must stay alive while a cast is running");
	expect(decide_auto_cast_monitor_action(true, AirPlayCastState::Stopped) ==
		       AutoCastMonitorAction::Scan,
	       "armed monitor must scan again after a receiver disconnect");

	std::cout << "airplay-receiver tests passed\n";
	return 0;
}
