#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <obs.h>
#include <util/threading.h>

#include "airplay-bridge.hpp"
#include "airplay-cast-state.hpp"

struct AirPlayCastSettings {
	std::string target = "抖音直播伴侣";
	std::string target_ip;
	uint16_t target_port = 0;
	std::string password;
	int fps = 60;
	int bitrate_kbps = 12000;
	int playout_latency_ms = 0;
	bool auto_cast_on_obs_start = true;
};

AirPlayCastSettings load_airplay_cast_settings();
void save_airplay_cast_settings(const AirPlayCastSettings &settings);

class AirPlayCast {
public:
	explicit AirPlayCast(std::wstring helper_path);
	~AirPlayCast();

	bool Start();
	bool WarmUp(std::string &error);
	void Stop();
	void Shutdown();

	AirPlayCastState State() const { return state_.load(); }
	bool Running() const { return state_.load() == AirPlayCastState::Running; }
	uint64_t TotalBytes() const { return video_bytes_.load(); }
	bool Start(const AirPlayCastSettings &settings);
	bool DiscoverDevices(std::vector<AirPlayDeviceInfo> &devices, const std::string &preferred, std::string &error);
	bool TakeError(std::string &message);

	void HandleEncodedPacket(const encoder_packet *packet);
	void HandleAudioData(const audio_data *audio);
	void HandleOutputStop();

private:
	void StartThread(AirPlayCastSettings settings);
	void MonitorSession();
	bool StartOutput(int receiver_width, int receiver_height, const AirPlayCastSettings &settings,
			 std::string &error);
	void StopOutput();
	void StoreError(const std::string &message);
	std::string ChooseEncoder() const;

	AirPlayBridge bridge_;
	std::atomic<AirPlayCastState> state_{AirPlayCastState::Stopped};
	std::thread start_thread_;
	std::thread monitor_thread_;
	std::mutex monitor_mutex_;
	std::condition_variable monitor_cv_;
	bool monitor_quit_ = false;

	obs_encoder_t *video_encoder_ = nullptr;
	obs_output_t *output_ = nullptr;
	signal_handler_t *output_signals_ = nullptr;
	os_event_t *stop_event_ = nullptr;
	bool audio_callback_registered_ = false;
	std::atomic<uint64_t> video_bytes_{0};
	std::chrono::steady_clock::time_point start_time_{};
	std::atomic<bool> first_video_seen_{false};
	std::atomic<bool> first_audio_seen_{false};
	std::mutex error_mutex_;
	std::string last_error_;
	bool error_pending_ = false;
};

void register_airplay_cast_output();
