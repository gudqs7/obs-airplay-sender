#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <windows.h>

struct AirPlayBridgeConfig {
	std::string target;
	std::string target_ip;
	uint16_t target_port = 0;
	std::string password;
	int fps = 60;
	int bitrate_kbps = 12000;
};

struct AirPlaySessionInfo {
	int width = 0;
	int height = 0;
	int audio_codec = 0;
};

struct AirPlayDeviceInfo {
	std::string name;
	std::string ip;
	uint16_t port = 0;
};

class AirPlayBridge {
public:
	explicit AirPlayBridge(std::wstring helper_path);
	~AirPlayBridge();

	bool WarmUp(std::string &error);
	bool StartSession(const AirPlayBridgeConfig &config, AirPlaySessionInfo &info, std::string &error);
	bool DiscoverDevices(std::vector<AirPlayDeviceInfo> &devices, const std::string &preferred,
			     std::string &error);
	void StopSession();
	void Shutdown();

	bool SyncClock(uint64_t obs_time_ns);
	bool PushVideo(const uint8_t *data, size_t size, uint64_t obs_time_ns);
	bool PushAudio(const uint8_t *data, size_t size, uint64_t obs_time_ns);
	bool Connected() const;

private:
	struct Packet {
		uint16_t type = 0;
		uint64_t pts = 0;
		std::vector<uint8_t> data;
	};

	bool EnsureHelper(std::string &error);
	bool ConnectSocket(uint16_t port, std::string &error);
	bool SendStart(const AirPlayBridgeConfig &config, AirPlaySessionInfo &info, std::string &error);
	bool Enqueue(uint16_t type, uint64_t pts, const uint8_t *data, size_t size, bool audio);
	void WriterLoop();
	bool SendPacket(uint16_t type, uint64_t pts, const uint8_t *data, size_t size);
	bool ReceiveResponse(std::string &payload, std::string &error);
	bool SendDiscover(std::vector<AirPlayDeviceInfo> &devices, const std::string &preferred,
			  std::string &error);
	bool ReadReady(uint16_t &port, std::string &error);
	void StartWriter();
	void StopWriter();
	void CloseSocket();
	void TerminateHelper();

	std::wstring helper_path_;
	HANDLE process_ = nullptr;
	HANDLE stdout_read_ = nullptr;
	HANDLE log_file_ = nullptr;
	SOCKET socket_ = INVALID_SOCKET;

	std::mutex queue_mutex_;
	std::condition_variable queue_cv_;
	std::deque<Packet> queue_;
	size_t queued_bytes_ = 0;
	bool writer_quit_ = false;
	std::atomic<bool> writer_running_{false};
	std::atomic<bool> writer_drop_logged_{false};
	std::thread writer_thread_;

	std::mutex io_mutex_;
	std::mutex operation_mutex_;
	bool winsock_ready_ = false;
};
