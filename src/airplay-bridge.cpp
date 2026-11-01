#include "airplay-bridge.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>

#include <obs-module.h>
#include <ws2tcpip.h>

namespace {

constexpr uint32_t kIpcMagic = 0x4f415043;
constexpr size_t kIpcHeaderBytes = 24;
constexpr uint16_t kIpcStart = 1;
constexpr uint16_t kIpcVideo = 2;
constexpr uint16_t kIpcAudio = 3;
constexpr uint16_t kIpcStop = 4;
constexpr uint16_t kIpcClock = 5;
constexpr uint16_t kIpcDiscover = 6;
constexpr uint16_t kIpcStatus = 7;
constexpr uint16_t kIpcResponse = 100;
constexpr size_t kMaxQueuedBytes = 64 * 1024 * 1024;

void put_u16(uint8_t *dst, uint16_t value)
{
	dst[0] = static_cast<uint8_t>(value);
	dst[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t *dst, uint32_t value)
{
	dst[0] = static_cast<uint8_t>(value);
	dst[1] = static_cast<uint8_t>(value >> 8);
	dst[2] = static_cast<uint8_t>(value >> 16);
	dst[3] = static_cast<uint8_t>(value >> 24);
}

void put_u64(uint8_t *dst, uint64_t value)
{
	for (size_t i = 0; i < 8; ++i)
		dst[i] = static_cast<uint8_t>(value >> (i * 8));
}

uint16_t get_u16(const uint8_t *src)
{
	return static_cast<uint16_t>(src[0] | (static_cast<uint16_t>(src[1]) << 8));
}

uint32_t get_u32(const uint8_t *src)
{
	return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
	       (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

uint64_t get_u64(const uint8_t *src)
{
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i)
		value |= static_cast<uint64_t>(src[i]) << (i * 8);
	return value;
}

bool send_all(SOCKET socket, const uint8_t *data, size_t size)
{
	while (size > 0) {
		int sent = send(socket, reinterpret_cast<const char *>(data),
				static_cast<int>(std::min<size_t>(size, 1u << 20)), 0);
		if (sent <= 0)
			return false;
		data += sent;
		size -= static_cast<size_t>(sent);
	}
	return true;
}

bool recv_all(SOCKET socket, uint8_t *data, size_t size)
{
	while (size > 0) {
		int received = recv(socket, reinterpret_cast<char *>(data),
				    static_cast<int>(std::min<size_t>(size, 1u << 20)), 0);
		if (received <= 0)
			return false;
		data += received;
		size -= static_cast<size_t>(received);
	}
	return true;
}

std::wstring quote_path(const std::wstring &path)
{
	return L"\"" + path + L"\"";
}

} // namespace

AirPlayBridge::AirPlayBridge(std::wstring helper_path) : helper_path_(std::move(helper_path))
{
	WSADATA data;
	winsock_ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

AirPlayBridge::~AirPlayBridge()
{
	Shutdown();
}

bool AirPlayBridge::EnsureHelper(std::string &error)
{
	if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT && socket_ != INVALID_SOCKET) {
		StartWriter();
		return true;
	}

	TerminateHelper();

	if (!winsock_ready_) {
		error = "Winsock initialization failed";
		return false;
	}

	SECURITY_ATTRIBUTES security = {};
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;

	HANDLE stdout_write = nullptr;
	if (!CreatePipe(&stdout_read_, &stdout_write, &security, 0)) {
		error = "Could not create helper stdout pipe";
		return false;
	}
	SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);

	HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
					&security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

	std::array<wchar_t, MAX_PATH> temp_path = {};
	GetTempPathW(static_cast<DWORD>(temp_path.size()), temp_path.data());
	std::wstring log_path = std::wstring(temp_path.data()) + L"obs-airplay-cast-bridge.log";
	log_file_ = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				&security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdOutput = stdout_write;
	startup.hStdError = log_file_ != INVALID_HANDLE_VALUE ? log_file_ : stdout_write;
	startup.hStdInput = null_input != INVALID_HANDLE_VALUE ? null_input : GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION process = {};
	std::wstring command_line = quote_path(helper_path_);
	std::filesystem::path working_directory = std::filesystem::path(helper_path_).parent_path();
	std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
	mutable_command.push_back(L'\0');

	BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
				      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
				      working_directory.wstring().c_str(), &startup, &process);

	CloseHandle(stdout_write);
	if (null_input != INVALID_HANDLE_VALUE)
		CloseHandle(null_input);

	if (!created) {
		error = "Could not start obs-airplay-bridge.exe";
		CloseHandle(stdout_read_);
		stdout_read_ = nullptr;
		if (log_file_ != INVALID_HANDLE_VALUE) {
			CloseHandle(log_file_);
			log_file_ = nullptr;
		}
		return false;
	}

	process_ = process.hProcess;
	CloseHandle(process.hThread);

	uint16_t port = 0;
	if (!ReadReady(port, error)) {
		TerminateHelper();
		return false;
	}
	CloseHandle(stdout_read_);
	stdout_read_ = nullptr;

	if (!ConnectSocket(port, error)) {
		TerminateHelper();
		return false;
	}

	StartWriter();
	return true;
}

bool AirPlayBridge::ReadReady(uint16_t &port, std::string &error)
{
	std::string output;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	while (std::chrono::steady_clock::now() < deadline) {
		DWORD available = 0;
		if (PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
			std::array<char, 256> buffer = {};
			DWORD read = 0;
			if (ReadFile(stdout_read_, buffer.data(),
				     std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
				     &read, nullptr)) {
				output.append(buffer.data(), read);
				const size_t newline = output.find('\n');
				if (newline != std::string::npos) {
					const std::string line = output.substr(0, newline);
					if (line.rfind("READY ", 0) == 0) {
						port = static_cast<uint16_t>(std::stoul(line.substr(6)));
						return port != 0;
					}
					error = "Unexpected bridge startup output: " + line;
					return false;
				}
			}
		}
		if (process_ && WaitForSingleObject(process_, 0) != WAIT_TIMEOUT) {
			error = "obs-airplay-bridge.exe exited during startup";
			return false;
		}
		Sleep(5);
	}
	error = "Timed out waiting for obs-airplay-bridge.exe";
	return false;
}

bool AirPlayBridge::ConnectSocket(uint16_t port, std::string &error)
{
	CloseSocket();
	socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_ == INVALID_SOCKET) {
		error = "Could not create bridge socket";
		return false;
	}

	DWORD timeout = 30000;
	setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
		   sizeof(timeout));
	setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout),
		   sizeof(timeout));

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
	if (connect(socket_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR) {
		error = "Could not connect to local AirPlay bridge";
		CloseSocket();
		return false;
	}

	BOOL no_delay = TRUE;
	setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&no_delay),
		   sizeof(no_delay));
	return true;
}

bool AirPlayBridge::WarmUp(std::string &error)
{
	std::lock_guard<std::mutex> operation(operation_mutex_);
	return EnsureHelper(error);
}

bool AirPlayBridge::StartSession(const AirPlayBridgeConfig &config, AirPlaySessionInfo &info,
				 std::string &error)
{
	std::lock_guard<std::mutex> operation(operation_mutex_);
	if (!EnsureHelper(error))
		return false;
	return SendStart(config, info, error);
}

bool AirPlayBridge::DiscoverDevices(std::vector<AirPlayDeviceInfo> &devices,
				    const std::string &preferred, std::string &error)
{
	std::lock_guard<std::mutex> operation(operation_mutex_);
	if (!EnsureHelper(error))
		return false;
	return SendDiscover(devices, preferred, error);
}

bool AirPlayBridge::SendDiscover(std::vector<AirPlayDeviceInfo> &devices,
				 const std::string &preferred, std::string &error)
{
	obs_data_t *request = obs_data_create();
	obs_data_set_string(request, "preferred", preferred.c_str());
	const char *json = obs_data_get_json(request);
	std::string payload = json ? json : "{}";
	obs_data_release(request);

	if (!SendPacket(kIpcDiscover, 0,
			reinterpret_cast<const uint8_t *>(payload.data()), payload.size())) {
		error = "Could not send discovery command to AirPlay bridge";
		return false;
	}

	std::string response;
	if (!ReceiveResponse(response, error))
		return false;

	obs_data_t *result = obs_data_create_from_json(response.c_str());
	if (!result) {
		error = "Invalid discovery response from AirPlay bridge";
		return false;
	}
	const bool ok = obs_data_get_bool(result, "ok");
	const char *message = obs_data_get_string(result, "message");
	if (!ok) {
		error = message && *message ? message : "AirPlay discovery failed";
		obs_data_release(result);
		return false;
	}

	devices.clear();
	obs_data_array_t *array = obs_data_get_array(result, "devices");
	const size_t count = array ? obs_data_array_count(array) : 0;
	for (size_t index = 0; index < count; ++index) {
		obs_data_t *item = obs_data_array_item(array, index);
		if (!item)
			continue;
		AirPlayDeviceInfo device;
		device.name = obs_data_get_string(item, "name");
		device.ip = obs_data_get_string(item, "ip");
		device.port = static_cast<uint16_t>(obs_data_get_int(item, "port"));
		obs_data_release(item);
		if (!device.name.empty() && !device.ip.empty())
			devices.push_back(std::move(device));
	}
	if (array)
		obs_data_array_release(array);
	obs_data_release(result);
	return true;
}

bool AirPlayBridge::SendStart(const AirPlayBridgeConfig &config, AirPlaySessionInfo &info,
			      std::string &error)
{
	obs_data_t *request = obs_data_create();
	obs_data_set_string(request, "target", config.target.c_str());
	obs_data_set_string(request, "targetIP", config.target_ip.c_str());
	obs_data_set_int(request, "targetPort", config.target_port);
	obs_data_set_string(request, "password", config.password.c_str());
	obs_data_set_int(request, "fps", config.fps);
	obs_data_set_int(request, "bitrate", config.bitrate_kbps);
	obs_data_set_int(request, "playoutLatencyMs", config.playout_latency_ms);
	const char *json = obs_data_get_json(request);
	std::string payload = json ? json : "{}";
	obs_data_release(request);

	if (!SendPacket(kIpcStart, 0, reinterpret_cast<const uint8_t *>(payload.data()), payload.size())) {
		error = "Could not send start command to AirPlay bridge";
		return false;
	}

	std::string response;
	if (!ReceiveResponse(response, error))
		return false;

	obs_data_t *result = obs_data_create_from_json(response.c_str());
	if (!result) {
		error = "Invalid response from AirPlay bridge";
		return false;
	}
	const bool ok = obs_data_get_bool(result, "ok");
	const char *message = obs_data_get_string(result, "message");
	if (!ok) {
		error = message && *message ? message : "AirPlay receiver setup failed";
		obs_data_release(result);
		return false;
	}

	info.width = static_cast<int>(obs_data_get_int(result, "width"));
	info.height = static_cast<int>(obs_data_get_int(result, "height"));
	info.audio_codec = static_cast<int>(obs_data_get_int(result, "audioCodec"));
	obs_data_release(result);
	return true;
}

bool AirPlayBridge::ReceiveResponse(std::string &payload, std::string &error, int timeout_ms)
{
	std::lock_guard<std::mutex> lock(io_mutex_);
	if (socket_ == INVALID_SOCKET) {
		error = "AirPlay bridge is not connected";
		return false;
	}

	DWORD timeout = static_cast<DWORD>(std::max(timeout_ms, 1));
	if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout),
		       sizeof(timeout)) == SOCKET_ERROR) {
		error = "Could not set AirPlay bridge response timeout";
		return false;
	}

	std::array<uint8_t, kIpcHeaderBytes> raw = {};
	const bool received = recv_all(socket_, raw.data(), raw.size());
	DWORD default_timeout = 30000;
	const auto restore_timeout = [&]() {
		setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
			   reinterpret_cast<const char *>(&default_timeout), sizeof(default_timeout));
	};
	if (!received) {
		restore_timeout();
		error = "Timed out waiting for AirPlay bridge";
		return false;
	}
	if (get_u32(raw.data()) != kIpcMagic || get_u16(raw.data() + 4) != kIpcResponse) {
		restore_timeout();
		error = "Invalid response header from AirPlay bridge";
		return false;
	}
	const uint32_t length = get_u32(raw.data() + 8);
	if (length > 1u << 20) {
		restore_timeout();
		error = "AirPlay bridge response is too large";
		return false;
	}
	payload.assign(length, '\0');
	if (length && !recv_all(socket_, reinterpret_cast<uint8_t *>(payload.data()), length)) {
		restore_timeout();
		error = "Could not read AirPlay bridge response";
		return false;
	}
	restore_timeout();
	return true;
}

bool AirPlayBridge::SessionStatus(AirPlaySessionStatus &status, std::string &error)
{
	std::lock_guard<std::mutex> operation(operation_mutex_);
	if (!Connected()) {
		error = "AirPlay bridge is not running";
		return false;
	}
	if (!SendPacket(kIpcStatus, 0, nullptr, 0)) {
		error = "Could not query AirPlay bridge session status";
		return false;
	}

	std::string response;
	if (!ReceiveResponse(response, error, 2000))
		return false;

	obs_data_t *result = obs_data_create_from_json(response.c_str());
	if (!result) {
		error = "Invalid session status response from AirPlay bridge";
		return false;
	}
	const bool ok = obs_data_get_bool(result, "ok");
	const char *session = obs_data_get_string(result, "session");
	const char *message = obs_data_get_string(result, "message");
	if (!ok) {
		error = message && *message ? message : "AirPlay bridge session status failed";
		obs_data_release(result);
		return false;
	}

	if (strcmp(session, "running") == 0) {
		status = AirPlaySessionStatus::Running;
	} else if (strcmp(session, "idle") == 0) {
		status = AirPlaySessionStatus::Idle;
	} else if (strcmp(session, "disconnected") == 0) {
		status = AirPlaySessionStatus::Disconnected;
		if (message && *message)
			error = message;
	} else if (strcmp(session, "stopped") == 0) {
		status = AirPlaySessionStatus::Stopped;
		if (message && *message)
			error = message;
	} else {
		error = "Unknown AirPlay bridge session status";
		obs_data_release(result);
		return false;
	}
	obs_data_release(result);
	return true;
}

bool AirPlayBridge::SyncClock(uint64_t obs_time_ns)
{
	return SendPacket(kIpcClock, obs_time_ns, nullptr, 0);
}

bool AirPlayBridge::PushVideo(const uint8_t *data, size_t size, uint64_t obs_time_ns)
{
	return Enqueue(kIpcVideo, obs_time_ns, data, size, false);
}

bool AirPlayBridge::PushAudio(const uint8_t *data, size_t size, uint64_t obs_time_ns)
{
	return Enqueue(kIpcAudio, obs_time_ns, data, size, true);
}

bool AirPlayBridge::Enqueue(uint16_t type, uint64_t pts, const uint8_t *data, size_t size,
			    bool audio)
{
	if (!data || size == 0 || socket_ == INVALID_SOCKET)
		return false;

	Packet packet;
	packet.type = type;
	packet.pts = pts;
	packet.data.assign(data, data + size);

	std::lock_guard<std::mutex> lock(queue_mutex_);
	if (writer_quit_ || !writer_running_.load()) {
		if (!writer_drop_logged_.exchange(true))
			blog(LOG_WARNING,
			     "[obs-airplay-cast] bridge packet dropped because the IPC writer is stopped");
		return false;
	}

	while (queued_bytes_ + packet.data.size() > kMaxQueuedBytes && !queue_.empty()) {
		if (audio) {
			auto video = std::find_if(queue_.begin(), queue_.end(),
						  [](const Packet &p) { return p.type == kIpcVideo; });
			if (video == queue_.end())
				break;
			queued_bytes_ -= video->data.size();
			queue_.erase(video);
		} else {
			queued_bytes_ -= queue_.front().data.size();
			queue_.pop_front();
		}
	}

	if (queued_bytes_ + packet.data.size() > kMaxQueuedBytes)
		return false;

	queued_bytes_ += packet.data.size();
	queue_.push_back(std::move(packet));
	queue_cv_.notify_one();
	return true;
}

void AirPlayBridge::WriterLoop()
{
	while (true) {
		Packet packet;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			queue_cv_.wait(lock, [this]() { return writer_quit_ || !queue_.empty(); });
			if (writer_quit_ && queue_.empty())
				break;
			packet = std::move(queue_.front());
			queue_.pop_front();
			queued_bytes_ -= packet.data.size();
		}

		if (!SendPacket(packet.type, packet.pts, packet.data.data(), packet.data.size())) {
			blog(LOG_WARNING, "[obs-airplay-cast] local bridge connection was closed");
			{
				std::lock_guard<std::mutex> lock(queue_mutex_);
				writer_quit_ = true;
				queue_.clear();
				queued_bytes_ = 0;
			}
			break;
		}
	}
	writer_running_ = false;
}

void AirPlayBridge::StartWriter()
{
	if (writer_running_.load() && writer_thread_.joinable())
		return;

	if (writer_thread_.joinable())
		writer_thread_.join();

	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		writer_quit_ = false;
		queue_.clear();
		queued_bytes_ = 0;
		writer_drop_logged_ = false;
		writer_running_ = true;
	}
	writer_thread_ = std::thread(&AirPlayBridge::WriterLoop, this);
	blog(LOG_INFO, "[obs-airplay-cast] bridge IPC writer started");
}

bool AirPlayBridge::SendPacket(uint16_t type, uint64_t pts, const uint8_t *data, size_t size)
{
	std::lock_guard<std::mutex> lock(io_mutex_);
	if (socket_ == INVALID_SOCKET)
		return false;

	std::array<uint8_t, kIpcHeaderBytes> raw = {};
	put_u32(raw.data(), kIpcMagic);
	put_u16(raw.data() + 4, type);
	put_u16(raw.data() + 6, 0);
	put_u32(raw.data() + 8, static_cast<uint32_t>(size));
	put_u32(raw.data() + 12, 0);
	put_u64(raw.data() + 16, pts);

	if (!send_all(socket_, raw.data(), raw.size()))
		return false;
	return !size || send_all(socket_, data, size);
}

void AirPlayBridge::StopWriter()
{
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		writer_quit_ = true;
		queue_.clear();
		queued_bytes_ = 0;
	}
	queue_cv_.notify_all();
	if (writer_thread_.joinable())
		writer_thread_.join();
	const bool was_running = writer_running_.exchange(false);
	if (was_running)
		blog(LOG_INFO, "[obs-airplay-cast] bridge IPC writer stopped");
}

void AirPlayBridge::StopSession()
{
	std::lock_guard<std::mutex> operation(operation_mutex_);
	StopWriter();
	if (socket_ == INVALID_SOCKET)
		return;

	if (SendPacket(kIpcStop, 0, nullptr, 0)) {
		std::string payload;
		std::string error;
		ReceiveResponse(payload, error);
	}
}

void AirPlayBridge::Shutdown()
{
	StopSession();
	CloseSocket();
	TerminateHelper();
	if (winsock_ready_) {
		WSACleanup();
		winsock_ready_ = false;
	}
}

bool AirPlayBridge::Connected() const
{
	return socket_ != INVALID_SOCKET && process_ &&
	       WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void AirPlayBridge::CloseSocket()
{
	if (socket_ != INVALID_SOCKET) {
		shutdown(socket_, SD_BOTH);
		closesocket(socket_);
		socket_ = INVALID_SOCKET;
	}
}

void AirPlayBridge::TerminateHelper()
{
	if (stdout_read_) {
		CloseHandle(stdout_read_);
		stdout_read_ = nullptr;
	}
	if (log_file_) {
		CloseHandle(log_file_);
		log_file_ = nullptr;
	}
	if (process_) {
		if (WaitForSingleObject(process_, 1500) == WAIT_TIMEOUT)
			TerminateProcess(process_, 1);
		CloseHandle(process_);
		process_ = nullptr;
	}
}
