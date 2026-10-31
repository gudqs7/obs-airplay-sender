#include "airplay-cast.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include "airplay-settings.hpp"

namespace {

constexpr const char *kTargetKey = "Target";
constexpr const char *kTargetIPKey = "TargetIP";
constexpr const char *kTargetPortKey = "TargetPort";
constexpr const char *kFPSKey = "FPS";
constexpr const char *kBitrateKey = "BitrateKbps";
constexpr const char *kAutoCastOnObsStartKey = "AutoCastOnObsStart";

struct AirPlayOutputContext {
	obs_output_t *output = nullptr;
	uint64_t packets = 0;
};

AirPlayCast *g_cast = nullptr;

long long elapsed_ms(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

int normalized_dimension(int value)
{
	value &= ~1;
	return value < 2 ? 2 : value;
}

std::pair<int, int> fitting_size(const obs_video_info &video, int receiver_width, int receiver_height)
{
	if (receiver_width <= 0 || receiver_height <= 0) {
		receiver_width = 1920;
		receiver_height = 1080;
	}
	if (video.base_width <= 0 || video.base_height <= 0)
		return {normalized_dimension(receiver_width), normalized_dimension(receiver_height)};

	const double scale = std::min({static_cast<double>(receiver_width) / video.base_width,
				       static_cast<double>(receiver_height) / video.base_height, 1.0});
	return {normalized_dimension(static_cast<int>(std::lround(video.base_width * scale))),
		normalized_dimension(static_cast<int>(std::lround(video.base_height * scale)))};
}

bool encoder_exists(const char *wanted)
{
	const char *id = nullptr;
	for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
		if (id && strcmp(id, wanted) == 0)
			return true;
	}
	return false;
}

void raw_audio_callback(void *param, size_t, audio_data *audio)
{
	if (param && audio)
		static_cast<AirPlayCast *>(param)->HandleAudioData(audio);
}

const char *airplay_output_name(void *)
{
	return obs_module_text("Output.Name");
}

void *airplay_output_create(obs_data_t *, obs_output_t *output)
{
	auto *context = new AirPlayOutputContext();
	context->output = output;
	return context;
}

void airplay_output_destroy(void *data)
{
	auto *context = static_cast<AirPlayOutputContext *>(data);
	if (!context)
		return;
	delete context;
}

bool airplay_output_start(void *data)
{
	auto *context = static_cast<AirPlayOutputContext *>(data);
	if (!obs_output_can_begin_data_capture(context->output, 0)) {
		blog(LOG_WARNING, "[obs-airplay-cast] output cannot begin data capture");
		return false;
	}
	if (!obs_output_initialize_encoders(context->output, 0)) {
		blog(LOG_WARNING, "[obs-airplay-cast] output could not initialize encoders");
		return false;
	}
	obs_output_begin_data_capture(context->output, 0);
	blog(LOG_INFO, "[obs-airplay-cast] output data capture began (active=%s)",
	     obs_output_active(context->output) ? "yes" : "no");
	return true;
}

void airplay_output_stop(void *data, uint64_t)
{
	auto *context = static_cast<AirPlayOutputContext *>(data);
	obs_output_end_data_capture(context->output);
}

void airplay_output_packet(void *data, encoder_packet *packet)
{
	auto *context = static_cast<AirPlayOutputContext *>(data);
	if (!packet) {
		obs_output_signal_stop(context->output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}
	if (context && context->packets < 3) {
		blog(LOG_INFO, "[obs-airplay-cast] output packet %llu: type=%d size=%zu keyframe=%s",
		     static_cast<unsigned long long>(context->packets), packet->type, packet->size,
		     packet->keyframe ? "yes" : "no");
		context->packets++;
	}
	if (g_cast)
		g_cast->HandleEncodedPacket(packet);
}

uint64_t airplay_output_total_bytes(void *data)
{
	UNUSED_PARAMETER(data);
	return g_cast ? g_cast->TotalBytes() : 0;
}

void output_stop_signal(void *data, calldata_t *)
{
	if (data)
		static_cast<AirPlayCast *>(data)->HandleOutputStop();
}

} // namespace

AirPlayCastSettings load_airplay_cast_settings()
{
	AirPlayCastSettings settings;
	config_t *config = obs_frontend_get_user_config();
	if (!config)
		return settings;

	const char *target = config_get_string(config, "AirPlayCast", kTargetKey);
	if (target && *target)
		settings.target = target;
	const char *target_ip = config_get_string(config, "AirPlayCast", kTargetIPKey);
	if (target_ip && *target_ip)
		settings.target_ip = target_ip;
	const int target_port = static_cast<int>(config_get_int(config, "AirPlayCast", kTargetPortKey));
	if (target_port > 0 && target_port <= 65535)
		settings.target_port = static_cast<uint16_t>(target_port);
	const int fps = static_cast<int>(config_get_int(config, "AirPlayCast", kFPSKey));
	if (fps == 30 || fps == 60)
		settings.fps = fps;
	const int bitrate = static_cast<int>(config_get_int(config, "AirPlayCast", kBitrateKey));
	if (bitrate >= 4000 && bitrate <= 30000)
		settings.bitrate_kbps = bitrate;
	settings.auto_cast_on_obs_start = resolve_auto_cast_on_obs_start(
		config_has_user_value(config, "AirPlayCast", kAutoCastOnObsStartKey),
		config_get_bool(config, "AirPlayCast", kAutoCastOnObsStartKey));
	return settings;
}

void save_airplay_cast_settings(const AirPlayCastSettings &settings)
{
	config_t *config = obs_frontend_get_user_config();
	if (!config)
		return;
	config_set_string(config, "AirPlayCast", kTargetKey, settings.target.c_str());
	config_set_string(config, "AirPlayCast", kTargetIPKey, settings.target_ip.c_str());
	config_set_int(config, "AirPlayCast", kTargetPortKey, settings.target_port);
	config_set_int(config, "AirPlayCast", kFPSKey, settings.fps);
	config_set_int(config, "AirPlayCast", kBitrateKey, settings.bitrate_kbps);
	config_set_bool(config, "AirPlayCast", kAutoCastOnObsStartKey, settings.auto_cast_on_obs_start);
	config_save(config);
}

AirPlayCast::AirPlayCast(std::wstring helper_path) : bridge_(std::move(helper_path))
{
	g_cast = this;
}

AirPlayCast::~AirPlayCast()
{
	Shutdown();
	if (g_cast == this)
		g_cast = nullptr;
}

bool AirPlayCast::Start()
{
	return Start(load_airplay_cast_settings());
}

bool AirPlayCast::Start(const AirPlayCastSettings &settings)
{
	const AirPlayCastState current = state_.load();
	if (current != AirPlayCastState::Stopped)
		return false;
	if (start_thread_.joinable())
		start_thread_.join();
	if (monitor_thread_.joinable())
		monitor_thread_.join();

	state_ = AirPlayCastState::Starting;
	start_time_ = std::chrono::steady_clock::now();
	first_video_seen_ = false;
	first_audio_seen_ = false;
	start_thread_ = std::thread(&AirPlayCast::StartThread, this, settings);
	return true;
}

bool AirPlayCast::WarmUp(std::string &error)
{
	return bridge_.WarmUp(error);
}

void AirPlayCast::StartThread(AirPlayCastSettings settings)
{
	blog(LOG_INFO, "[obs-airplay-cast] starting receiver '%s' at %s:%u", settings.target.c_str(),
	     settings.target_ip.c_str(), settings.target_port);

	AirPlayBridgeConfig bridge_config;
	bridge_config.target = settings.target;
	bridge_config.target_ip = settings.target_ip;
	bridge_config.target_port = settings.target_port;
	bridge_config.password = settings.password;
	bridge_config.fps = settings.fps;
	bridge_config.bitrate_kbps = settings.bitrate_kbps;

	AirPlaySessionInfo info;
	std::string error;
	const auto setup_started = std::chrono::steady_clock::now();
	if (!bridge_.StartSession(bridge_config, info, error)) {
		blog(LOG_WARNING, "[obs-airplay-cast] receiver setup failed: %s", error.c_str());
		StoreError(error);
		state_ = AirPlayCastState::Stopped;
		return;
	}
	blog(LOG_INFO, "[obs-airplay-cast] timing: AirPlay ready in %lld ms",
	     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
					    std::chrono::steady_clock::now() - setup_started)
					    .count()));

	const auto output_started = std::chrono::steady_clock::now();
	if (!StartOutput(info.width, info.height, settings, error)) {
		blog(LOG_WARNING, "[obs-airplay-cast] OBS encoder failed: %s", error.c_str());
		StoreError(error);
		bridge_.StopSession();
		state_ = AirPlayCastState::Stopped;
		return;
	}
	blog(LOG_INFO, "[obs-airplay-cast] timing: OBS output started in %lld ms",
	     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
					    std::chrono::steady_clock::now() - output_started)
					    .count()));

	state_ = AirPlayCastState::Running;
	blog(LOG_INFO, "[obs-airplay-cast] streaming to AirPlay receiver '%s' (%dx%d, %d kbps)",
	     settings.target.c_str(), info.width, info.height, settings.bitrate_kbps);
	blog(LOG_INFO, "[obs-airplay-cast] timing: click to output ready %lld ms", elapsed_ms(start_time_));
	{
		std::lock_guard<std::mutex> lock(monitor_mutex_);
		monitor_quit_ = false;
	}
	monitor_thread_ = std::thread(&AirPlayCast::MonitorSession, this);
}

void AirPlayCast::MonitorSession()
{
	while (true) {
		std::unique_lock<std::mutex> lock(monitor_mutex_);
		if (monitor_cv_.wait_for(lock, std::chrono::seconds(1), [this]() { return monitor_quit_; }))
			return;
		lock.unlock();

		if (state_.load() != AirPlayCastState::Running)
			return;

		AirPlaySessionStatus status = AirPlaySessionStatus::Idle;
		std::string error;
		const bool transport_ok = bridge_.SessionStatus(status, error);
		if (state_.load() != AirPlayCastState::Running)
			return;
		if (transport_ok && status == AirPlaySessionStatus::Running)
			continue;

		AirPlayCastState expected = AirPlayCastState::Running;
		if (!state_.compare_exchange_strong(expected, AirPlayCastState::Stopping))
			return;

		const std::string reason =
			!transport_ok ? error
				      : (error.empty() ? "AirPlay receiver session ended" : error);
		blog(LOG_WARNING, "[obs-airplay-cast] receiver disconnected: %s", reason.c_str());
		StoreError("与抖音直播伴侣的连接已断开：" + reason);
		StopOutput();
		bridge_.StopSession();
		state_ = AirPlayCastState::Stopped;
		monitor_cv_.notify_all();
		return;
	}
}

std::string AirPlayCast::ChooseEncoder() const
{
	static const std::array<const char *, 5> preferred = {"obs_nvenc_h264_tex", "obs_nvenc_h264_cuda",
							      "obs_qsv11_v2", "obs_qsv11", "obs_x264"};
	for (const char *id : preferred) {
		if (encoder_exists(id))
			return id;
	}

	const char *id = nullptr;
	for (size_t index = 0; obs_enum_encoder_types(index, &id); ++index) {
		if (id && obs_get_encoder_type(id) == OBS_ENCODER_VIDEO &&
		    strcmp(obs_get_encoder_codec(id), "h264") == 0)
			return id;
	}
	return {};
}

bool AirPlayCast::StartOutput(int receiver_width, int receiver_height, const AirPlayCastSettings &settings,
			      std::string &error)
{
	obs_video_info video_info = {};
	if (!obs_get_video_info(&video_info)) {
		error = "OBS video is not initialized";
		return false;
	}

	const auto [width, height] = fitting_size(video_info, receiver_width, receiver_height);
	const std::string encoder_id = ChooseEncoder();
	if (encoder_id.empty()) {
		error = "no H.264 encoder is installed";
		return false;
	}

	obs_data_t *encoder_settings = obs_encoder_defaults(encoder_id.c_str());
	obs_data_set_int(encoder_settings, "bitrate", settings.bitrate_kbps);
	obs_data_set_string(encoder_settings, "rate_control", "CBR");
	obs_data_set_int(encoder_settings, "keyint_sec", 1);
	obs_data_set_string(encoder_settings, "profile", "high");
	if (encoder_id == "obs_x264") {
		obs_data_set_string(encoder_settings, "preset", "veryfast");
		obs_data_set_string(encoder_settings, "tune", "zerolatency");
		obs_data_set_bool(encoder_settings, "repeat_headers", true);
	} else if (encoder_id.find("nvenc") != std::string::npos) {
		obs_data_set_string(encoder_settings, "preset", "p5");
		// AirPlay mirroring consumes access units in wire order. B-frames and
		// lookahead create decode-order timestamps and add latency, which makes
		// Douyin's receiver present only about one third of a 60 fps stream.
		obs_data_set_string(encoder_settings, "tune", "ull");
		obs_data_set_int(encoder_settings, "bf", 0);
		obs_data_set_bool(encoder_settings, "lookahead", false);
		obs_data_set_string(encoder_settings, "multipass", "disabled");
	}

	video_encoder_ = obs_video_encoder_create(encoder_id.c_str(), "obs-airplay-h264", encoder_settings, nullptr);
	obs_data_release(encoder_settings);
	if (!video_encoder_) {
		error = "could not create H.264 encoder " + encoder_id;
		return false;
	}

	// Douyin renders AirPlay video into a partial-range NV12 surface.
	obs_encoder_set_gpu_scale_type(video_encoder_, OBS_SCALE_BILINEAR);
	obs_encoder_set_preferred_color_space(video_encoder_, VIDEO_CS_709);
	obs_encoder_set_preferred_range(video_encoder_, VIDEO_RANGE_PARTIAL);

	obs_encoder_set_video(video_encoder_, obs_get_video());
	obs_encoder_set_scaled_size(video_encoder_, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	const double base_fps = video_info.fps_den ? static_cast<double>(video_info.fps_num) / video_info.fps_den : 0;
	const int requested_fps = settings.fps;
	const uint32_t divisor =
		base_fps > requested_fps
			? std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(base_fps / requested_fps)))
			: 1;
	obs_encoder_set_frame_rate_divisor(video_encoder_, divisor);

	output_ = obs_output_create("obs_airplay_cast_output", "AirPlay Cast", nullptr, nullptr);
	if (!output_) {
		error = "could not create AirPlay output";
		obs_encoder_release(video_encoder_);
		video_encoder_ = nullptr;
		return false;
	}
	obs_output_set_video_encoder(output_, video_encoder_);

	if (os_event_init(&stop_event_, OS_EVENT_TYPE_AUTO) != 0) {
		error = "could not create output stop event";
		StopOutput();
		return false;
	}
	output_signals_ = obs_output_get_signal_handler(output_);
	signal_handler_connect(output_signals_, "deactivate", output_stop_signal, this);

	audio_convert_info conversion = {};
	conversion.samples_per_sec = 44100;
	conversion.format = AUDIO_FORMAT_16BIT;
	conversion.speakers = SPEAKERS_STEREO;
	conversion.allow_clipping = false;
	obs_add_raw_audio_callback(0, &conversion, raw_audio_callback, this);
	audio_callback_registered_ = true;

	bridge_.SyncClock(os_gettime_ns());
	state_ = AirPlayCastState::Running;
	if (!obs_output_start(output_)) {
		state_ = AirPlayCastState::Starting;
		error = "OBS output failed to start";
		StopOutput();
		return false;
	}
	return true;
}

void AirPlayCast::StopOutput()
{
	if (audio_callback_registered_) {
		obs_remove_raw_audio_callback(0, raw_audio_callback, this);
		audio_callback_registered_ = false;
	}
	if (output_) {
		const bool active = obs_output_active(output_);
		if (active)
			obs_output_stop(output_);
		if (active && stop_event_ && os_event_timedwait(stop_event_, 3000) != 0) {
			blog(LOG_WARNING, "[obs-airplay-cast] output deactivate timed out; forcing OBS output stop");
			obs_output_force_stop(output_);
		}
		if (output_signals_)
			signal_handler_disconnect(output_signals_, "deactivate", output_stop_signal, this);
		// The deactivate signal precedes output destruction. Releasing the output
		// is the final barrier that joins OBS's end-data-capture thread.
		obs_output_release(output_);
		output_ = nullptr;
		output_signals_ = nullptr;
	}
	if (stop_event_) {
		os_event_destroy(stop_event_);
		stop_event_ = nullptr;
	}
	if (video_encoder_) {
		obs_encoder_release(video_encoder_);
		video_encoder_ = nullptr;
	}
	video_bytes_ = 0;
}

void AirPlayCast::Stop()
{
	const AirPlayCastState current = state_.load();
	if (current == AirPlayCastState::Stopped && !start_thread_.joinable() && !monitor_thread_.joinable())
		return;

	if (current != AirPlayCastState::Stopping)
		state_ = AirPlayCastState::Stopping;
	{
		std::lock_guard<std::mutex> lock(monitor_mutex_);
		monitor_quit_ = true;
	}
	monitor_cv_.notify_all();
	if (start_thread_.joinable())
		start_thread_.join();
	if (monitor_thread_.joinable())
		monitor_thread_.join();
	StopOutput();
	bridge_.StopSession();
	state_ = AirPlayCastState::Stopped;
	blog(LOG_INFO, "[obs-airplay-cast] stopped");
}

void AirPlayCast::Shutdown()
{
	Stop();
	bridge_.Shutdown();
}

void AirPlayCast::HandleEncodedPacket(const encoder_packet *packet)
{
	if (!packet || state_.load() != AirPlayCastState::Running || packet->type != OBS_ENCODER_VIDEO ||
	    !packet->data || packet->size == 0)
		return;

	const uint64_t timestamp = packet->sys_dts_usec > 0 ? static_cast<uint64_t>(packet->sys_dts_usec) * 1000ULL
							    : os_gettime_ns();
	if (bridge_.PushVideo(packet->data, packet->size, timestamp)) {
		bool expected = false;
		if (first_video_seen_.compare_exchange_strong(expected, true)) {
			blog(LOG_INFO, "[obs-airplay-cast] timing: click to first encoded video %lld ms (keyframe=%s)",
			     elapsed_ms(start_time_), packet->keyframe ? "yes" : "no");
		}
		video_bytes_ += packet->size;
	}
}

void AirPlayCast::HandleAudioData(const audio_data *audio)
{
	if (!audio || state_.load() != AirPlayCastState::Running || !audio->data[0])
		return;
	const size_t bytes = static_cast<size_t>(audio->frames) * 2 * sizeof(int16_t);
	if (bridge_.PushAudio(audio->data[0], bytes, audio->timestamp)) {
		bool expected = false;
		if (first_audio_seen_.compare_exchange_strong(expected, true))
			blog(LOG_INFO, "[obs-airplay-cast] timing: click to first audio callback %lld ms",
			     elapsed_ms(start_time_));
	}
}

void AirPlayCast::HandleOutputStop()
{
	if (stop_event_)
		os_event_signal(stop_event_);
}

bool AirPlayCast::DiscoverDevices(std::vector<AirPlayDeviceInfo> &devices, const std::string &preferred,
				  std::string &error)
{
	if (state_.load() != AirPlayCastState::Stopped) {
		error = "停止投屏后才能重新扫描接收端";
		return false;
	}
	return bridge_.DiscoverDevices(devices, preferred, error);
}

void AirPlayCast::StoreError(const std::string &message)
{
	std::lock_guard<std::mutex> lock(error_mutex_);
	last_error_ = message;
	error_pending_ = true;
}

bool AirPlayCast::TakeError(std::string &message)
{
	std::lock_guard<std::mutex> lock(error_mutex_);
	if (!error_pending_)
		return false;
	message = last_error_;
	last_error_.clear();
	error_pending_ = false;
	return true;
}

void register_airplay_cast_output()
{
	static obs_output_info info = {};
	info.id = "obs_airplay_cast_output";
	info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	info.get_name = airplay_output_name;
	info.create = airplay_output_create;
	info.destroy = airplay_output_destroy;
	info.start = airplay_output_start;
	info.stop = airplay_output_stop;
	info.encoded_packet = airplay_output_packet;
	info.get_total_bytes = airplay_output_total_bytes;
	info.encoded_video_codecs = "h264";
	obs_register_output(&info);
}
