#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <plugin-support.h>

#include "airplay-auto-cast.hpp"
#include "airplay-cast.hpp"
#include "airplay-receiver.hpp"
#include "airplay-settings.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

struct ReceiverScanResult {
	bool ok = false;
	std::vector<AirPlayDeviceInfo> devices;
	std::string error;
	long long elapsed_ms = 0;
};

std::unique_ptr<AirPlayCast> g_airplay_cast;
std::future<ReceiverScanResult> g_auto_cast_future;
std::unique_ptr<QTimer> g_auto_cast_timer;
AirPlayCastSettings g_auto_cast_settings;
std::atomic<bool> g_auto_cast_armed{false};
std::chrono::steady_clock::time_point g_last_auto_cast_warning{};

std::wstring helper_path()
{
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&obs_module_load), &module);
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
	return (std::filesystem::path(path).parent_path() / L"obs-airplay-cast-bridge.exe").wstring();
}

ReceiverScanResult run_receiver_scan(AirPlayCast *cast, std::string preferred)
{
	ReceiverScanResult result;
	const auto scan_started = std::chrono::steady_clock::now();
	if (cast) {
		result.ok = cast->DiscoverDevices(result.devices, preferred, result.error);
	} else {
		result.error = "AirPlay cast is not initialized";
	}
	result.elapsed_ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - scan_started)
			.count();
	return result;
}

void StopAutoCastMonitor()
{
	const bool was_armed = g_auto_cast_armed.exchange(false);
	if (g_auto_cast_timer)
		g_auto_cast_timer->stop();
	if (g_auto_cast_future.valid()) {
		try {
			(void)g_auto_cast_future.get();
		} catch (...) {
		}
	}
	if (was_armed)
		blog(LOG_INFO, "[obs-airplay-cast] auto-cast monitor stopped");
}

void LaunchAutoCastPoll()
{
	if (!g_auto_cast_armed.load() || g_auto_cast_future.valid() || !g_airplay_cast)
		return;
	try {
		g_auto_cast_future = std::async(std::launch::async, [cast = g_airplay_cast.get()]() {
			return run_receiver_scan(cast, "抖音直播伴侣");
		});
	} catch (const std::exception &error) {
		g_auto_cast_armed = false;
		if (g_auto_cast_timer)
			g_auto_cast_timer->stop();
		blog(LOG_WARNING, "[obs-airplay-cast] could not start auto-cast discovery: %s", error.what());
	}
}

void PollAutoCastMonitor()
{
	if (!g_auto_cast_armed.load()) {
		if (g_auto_cast_timer)
			g_auto_cast_timer->stop();
		return;
	}
	if (!g_airplay_cast) {
		StopAutoCastMonitor();
		return;
	}
	const AutoCastMonitorAction action =
		decide_auto_cast_monitor_action(true, g_airplay_cast->State());
	if (action == AutoCastMonitorAction::None) {
		StopAutoCastMonitor();
		return;
	}
	if (action == AutoCastMonitorAction::Wait)
		return;
	if (!g_auto_cast_future.valid())
		LaunchAutoCastPoll();
	if (!g_auto_cast_future.valid())
		return;
	if (g_auto_cast_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
		return;

	ReceiverScanResult result;
	try {
		result = g_auto_cast_future.get();
	} catch (const std::exception &error) {
		result.error = error.what();
	} catch (...) {
		result.error = "unknown auto-cast discovery error";
	}

	if (!g_auto_cast_armed.load())
		return;

	blog(LOG_INFO, "[obs-airplay-cast] auto-cast discovery: devices=%zu elapsed=%lld ms%s%s", result.devices.size(),
	     result.elapsed_ms, result.ok ? "" : " error=", result.ok ? "" : result.error.c_str());

	if (result.ok) {
		const AirPlayDeviceInfo *receiver = find_douyin_receiver(result.devices, g_auto_cast_settings.target);
		if (receiver) {
			AirPlayCastSettings settings = g_auto_cast_settings;
			settings.target = receiver->name;
			settings.target_ip = receiver->ip;
			settings.target_port = receiver->port;

			save_airplay_cast_settings(settings);
			blog(LOG_INFO, "[obs-airplay-cast] auto-cast found receiver '%s' at %s:%u",
			     settings.target.c_str(), settings.target_ip.c_str(), settings.target_port);
			if (!g_airplay_cast->Start(settings)) {
				blog(LOG_WARNING, "[obs-airplay-cast] auto-cast could not start receiver '%s'",
				     settings.target.c_str());
			}
			g_auto_cast_settings = std::move(settings);
			return;
		}
	} else {
		const auto now = std::chrono::steady_clock::now();
		if (g_last_auto_cast_warning.time_since_epoch().count() == 0 ||
		    now - g_last_auto_cast_warning >= std::chrono::seconds(30)) {
			blog(LOG_WARNING, "[obs-airplay-cast] auto-cast discovery failed: %s", result.error.c_str());
			g_last_auto_cast_warning = now;
		}
	}

	LaunchAutoCastPoll();
}

void StartAutoCastMonitor()
{
	if (g_auto_cast_armed.load() || !g_airplay_cast)
		return;

	AirPlayCastSettings settings = load_airplay_cast_settings();
	if (!settings.auto_cast_on_obs_start)
		return;

	g_auto_cast_settings = std::move(settings);
	g_last_auto_cast_warning = {};
	g_auto_cast_armed = true;
	if (!g_auto_cast_timer) {
		g_auto_cast_timer = std::make_unique<QTimer>();
		g_auto_cast_timer->setInterval(1000);
		g_auto_cast_timer->setTimerType(Qt::CoarseTimer);
		QObject::connect(g_auto_cast_timer.get(), &QTimer::timeout, g_auto_cast_timer.get(),
				 []() { PollAutoCastMonitor(); });
	}
	g_auto_cast_timer->start();
	blog(LOG_INFO, "[obs-airplay-cast] auto-cast monitor started");
	if (g_airplay_cast->State() == AirPlayCastState::Stopped)
		LaunchAutoCastPoll();
}

bool AutoCastWaiting()
{
	return g_auto_cast_armed.load();
}

void show_settings_dialog()
{
	AirPlayCastSettings settings = load_airplay_cast_settings();

	QDialog dialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	dialog.setWindowTitle(obs_module_text("Settings.Title"));
	dialog.setMinimumWidth(520);

	auto *layout = new QVBoxLayout(&dialog);
	auto *form = new QFormLayout();

	auto *target = new QComboBox();
	target->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	target->setMinimumContentsLength(28);
	if (is_douyin_receiver_name(settings.target)) {
		target->addItem(QString::fromUtf8(settings.target.c_str()), QString::fromUtf8(settings.target.c_str()));
		target->setItemData(0, QString::fromUtf8(settings.target_ip.c_str()), Qt::UserRole + 1);
		target->setItemData(0, settings.target_port, Qt::UserRole + 2);
	}

	auto *scan_button = new QToolButton();
	scan_button->setAutoRaise(true);
	scan_button->setIcon(scan_button->style()->standardIcon(QStyle::SP_BrowserReload));
	scan_button->setToolTip(obs_module_text("Settings.Scan"));
	auto *device_row = new QWidget();
	auto *device_layout = new QHBoxLayout(device_row);
	device_layout->setContentsMargins(0, 0, 0, 0);
	device_layout->addWidget(target, 1);
	device_layout->addWidget(scan_button);
	form->addRow(obs_module_text("Settings.Target"), device_row);

	auto *fps = new QComboBox();
	fps->addItem(QStringLiteral("60 fps"), 60);
	fps->addItem(QStringLiteral("30 fps"), 30);
	fps->setCurrentIndex(settings.fps == 30 ? 1 : 0);
	form->addRow(obs_module_text("Settings.FPS"), fps);

	auto *bitrate = new QSpinBox();
	bitrate->setRange(4000, 30000);
	bitrate->setSingleStep(1000);
	bitrate->setSuffix(QStringLiteral(" kbps"));
	bitrate->setValue(settings.bitrate_kbps);
	form->addRow(obs_module_text("Settings.Bitrate"), bitrate);

	auto *playout_latency = new QSpinBox();
	playout_latency->setRange(0, 500);
	playout_latency->setSingleStep(10);
	playout_latency->setKeyboardTracking(false);
	playout_latency->setSuffix(QStringLiteral(" ms"));
	playout_latency->setSpecialValueText(obs_module_text("Settings.LatencyAuto"));
	playout_latency->setToolTip(obs_module_text("Settings.LatencyTooltip"));
	playout_latency->setValue(settings.playout_latency_ms);
	QObject::connect(playout_latency, qOverload<int>(&QSpinBox::valueChanged), playout_latency,
			 [playout_latency](int value) {
				 if (value > 0 && value < 80)
					 playout_latency->setValue(80);
			 });
	form->addRow(obs_module_text("Settings.Latency"), playout_latency);

	auto *auto_cast = new QCheckBox(obs_module_text("Settings.AutoCastOnObsStart"));
	auto_cast->setChecked(settings.auto_cast_on_obs_start);
	form->addRow(auto_cast);

	layout->addLayout(form);

	auto *scan_status = new QLabel();
	scan_status->setWordWrap(true);
	layout->addWidget(scan_status);

	auto *cast_status = new QLabel();
	cast_status->setWordWrap(true);
	layout->addWidget(cast_status);

	auto *button_row = new QHBoxLayout();
	button_row->addStretch();
	auto *cast_button = new QPushButton(obs_module_text("Action.Start"));
	cast_button->setMinimumWidth(130);
	auto *close_button = new QPushButton(obs_module_text("Action.Close"));
	button_row->addWidget(cast_button);
	button_row->addWidget(close_button);
	layout->addLayout(button_row);

	auto scan_future = std::make_shared<std::future<ReceiverScanResult>>();
	auto scanning = std::make_shared<std::atomic<bool>>(false);
	const QString preferred = is_douyin_receiver_name(settings.target) ? QString::fromUtf8(settings.target.c_str())
									   : QString();
	auto finish_scan = [=](ReceiverScanResult result) {
		if (result.ok) {
			std::stable_sort(result.devices.begin(), result.devices.end(),
					 [](const AirPlayDeviceInfo &left, const AirPlayDeviceInfo &right) {
						 const bool left_douyin = is_douyin_receiver_name(left.name);
						 const bool right_douyin = is_douyin_receiver_name(right.name);
						 if (left_douyin != right_douyin)
							 return left_douyin;
						 return left.name < right.name;
					 });
		}
		blog(LOG_INFO, "[obs-airplay-cast] scan completed: devices=%zu elapsed=%lld ms%s%s",
		     result.devices.size(), result.elapsed_ms,
		     result.ok ? "" : " error=", result.ok ? "" : result.error.c_str());

		target->clear();
		int select_index = -1;
		int preferred_index = -1;
		for (const AirPlayDeviceInfo &device : result.devices) {
			const QString name = QString::fromUtf8(device.name.c_str());
			const QString display =
				QStringLiteral("%1  (%2)").arg(name, QString::fromUtf8(device.ip.c_str()));
			target->addItem(display, name);
			target->setItemData(target->count() - 1, QString::fromUtf8(device.ip.c_str()),
					    Qt::UserRole + 1);
			target->setItemData(target->count() - 1, static_cast<int>(device.port), Qt::UserRole + 2);
			const int item_index = target->count() - 1;
			if (name == preferred)
				preferred_index = item_index;
			if (select_index < 0 && is_douyin_receiver_name(device.name))
				select_index = item_index;
		}
		target->setCurrentIndex(preferred_index >= 0 ? preferred_index : select_index);

		if (!result.ok) {
			scan_status->setVisible(true);
			scan_status->setText(QString::fromUtf8(result.error.c_str()));
		} else if (result.devices.empty()) {
			scan_status->setVisible(true);
			scan_status->setText(obs_module_text("Settings.NoDevices"));
		} else {
			scan_status->setVisible(true);
			scan_status->setText(
				QString::fromUtf8(obs_module_text("Settings.Found")).arg(result.devices.size()));
		}
		scan_button->setEnabled(true);
		cast_button->setEnabled(target->currentIndex() >= 0);
		scanning->store(false);
	};
	auto poll_scan = [=]() {
		if (!scan_future->valid() ||
		    scan_future->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
			return;

		ReceiverScanResult result;
		try {
			result = scan_future->get();
		} catch (const std::exception &error) {
			result.error = error.what();
		} catch (...) {
			result.error = "unknown receiver discovery error";
		}
		if (!g_airplay_cast || g_airplay_cast->State() != AirPlayCastState::Stopped) {
			scanning->store(false);
			return;
		}
		finish_scan(std::move(result));
	};
	auto scan = [=]() {
		if (!g_airplay_cast || g_airplay_cast->State() != AirPlayCastState::Stopped)
			return;
		if (scanning->exchange(true))
			return;
		scan_button->setEnabled(false);
		scan_status->setVisible(true);
		scan_status->setText(obs_module_text("Settings.Scanning"));
		try {
			*scan_future = std::async(std::launch::async, [cast = g_airplay_cast.get(),
								       receiver = preferred.toUtf8().toStdString()]() {
				return run_receiver_scan(cast, receiver);
			});
		} catch (const std::exception &error) {
			scanning->store(false);
			scan_button->setEnabled(true);
			scan_status->setText(QString::fromUtf8(error.what()));
		}
	};
	QObject::connect(scan_button, &QToolButton::clicked, &dialog, scan);

	auto persist_settings = [&]() {
		if (target->currentIndex() >= 0) {
			settings.target = target->currentData().toString().toUtf8().toStdString();
			settings.target_ip = target->currentData(Qt::UserRole + 1).toString().toUtf8().toStdString();
			settings.target_port = static_cast<uint16_t>(target->currentData(Qt::UserRole + 2).toInt());
		}
		settings.fps = fps->currentData().toInt();
		settings.bitrate_kbps = bitrate->value();
		settings.playout_latency_ms = playout_latency->value();
		settings.auto_cast_on_obs_start = auto_cast->isChecked();
		save_airplay_cast_settings(settings);
	};
	auto apply_settings = [&]() -> bool {
		if (target->currentIndex() < 0)
			return false;
		persist_settings();
		return true;
	};

	auto set_inputs_enabled = [&](bool enabled) {
		target->setEnabled(enabled);
		scan_button->setEnabled(enabled);
		fps->setEnabled(enabled);
		bitrate->setEnabled(enabled);
		playout_latency->setEnabled(enabled);
		auto_cast->setEnabled(enabled);
	};

	auto refresh_state = [&]() {
		if (!g_airplay_cast)
			return;
		const AirPlayCastState state = g_airplay_cast->State();
		switch (state) {
		case AirPlayCastState::Stopped:
			set_inputs_enabled(true);
			cast_button->setEnabled(target->currentIndex() >= 0);
			cast_button->setText(obs_module_text("Action.Start"));
			cast_status->setText(obs_module_text(AutoCastWaiting() ? "Status.AutoWaiting" : "Status.Idle"));
			break;
		case AirPlayCastState::Starting:
			set_inputs_enabled(false);
			cast_button->setEnabled(false);
			cast_button->setText(obs_module_text("Action.Starting"));
			cast_status->setText(obs_module_text("Status.Connecting"));
			break;
		case AirPlayCastState::Running:
			set_inputs_enabled(false);
			cast_button->setEnabled(true);
			cast_button->setText(obs_module_text("Action.Stop"));
			cast_status->setText(obs_module_text("Status.Running"));
			break;
		case AirPlayCastState::Stopping:
			set_inputs_enabled(false);
			cast_button->setEnabled(false);
			cast_button->setText(obs_module_text("Action.Stopping"));
			cast_status->setText(obs_module_text("Status.Stopping"));
			break;
		}

		std::string error;
		if (g_airplay_cast->TakeError(error)) {
			QMessageBox::warning(&dialog, obs_module_text("Error.Title"), QString::fromUtf8(error.c_str()));
		}
	};

	QObject::connect(cast_button, &QPushButton::clicked, &dialog, [&]() {
		if (!g_airplay_cast)
			return;
		if (g_airplay_cast->State() == AirPlayCastState::Running) {
			StopAutoCastMonitor();
			g_airplay_cast->Stop();
			refresh_state();
			return;
		}
		if (g_airplay_cast->State() != AirPlayCastState::Stopped)
			return;
		if (!apply_settings()) {
			QMessageBox::warning(&dialog, obs_module_text("Error.Title"),
					     obs_module_text("Settings.NoDevices"));
			return;
		}
		if (auto_cast->isChecked())
			StartAutoCastMonitor();
		else
			StopAutoCastMonitor();
		set_inputs_enabled(false);
		cast_button->setEnabled(false);
		cast_status->setText(obs_module_text("Status.Connecting"));
		g_airplay_cast->Start();
	});

	QObject::connect(close_button, &QPushButton::clicked, &dialog, [&]() {
		if (g_airplay_cast->State() == AirPlayCastState::Stopped)
			persist_settings();
		if (auto_cast->isChecked())
			StartAutoCastMonitor();
		else
			StopAutoCastMonitor();
		dialog.accept();
	});

	auto *state_timer = new QTimer(&dialog);
	state_timer->setInterval(250);
	QObject::connect(state_timer, &QTimer::timeout, &dialog, [&]() {
		poll_scan();
		refresh_state();
	});
	state_timer->start();

	if (!g_airplay_cast) {
		scan_button->setEnabled(false);
		scan_status->setVisible(false);
	} else {
		const bool cast_stopped = g_airplay_cast->State() == AirPlayCastState::Stopped;
		switch (decide_initial_scan_action(cast_stopped, AutoCastWaiting())) {
		case InitialScanAction::WaitForAutoCast:
			// The auto-cast wait state is owned by cast_status. Keeping this
			// label empty prevents the same status from appearing twice.
			scan_status->setVisible(false);
			break;
		case InitialScanAction::Scan:
			scan();
			break;
		case InitialScanAction::ShowStopBeforeScan:
			scan_button->setEnabled(false);
			scan_status->setVisible(true);
			scan_status->setText(obs_module_text("Settings.StopBeforeScan"));
			break;
		}
	}

	refresh_state();
	dialog.exec();
}

void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		StartAutoCastMonitor();
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		StopAutoCastMonitor();
		if (g_airplay_cast)
			g_airplay_cast->Shutdown();
	}
}

} // namespace

bool obs_module_load(void)
{
	register_airplay_cast_output();
	g_airplay_cast = std::make_unique<AirPlayCast>(helper_path());
	std::string warmup_error;
	if (!g_airplay_cast->WarmUp(warmup_error)) {
		blog(LOG_WARNING, "[obs-airplay-cast] bridge prewarm failed: %s", warmup_error.c_str());
	}

	QAction *settings_action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Tools.Settings")));

	if (settings_action)
		QObject::connect(settings_action, &QAction::triggered, []() { show_settings_dialog(); });

	obs_frontend_add_event_callback(frontend_event, nullptr);
	blog(LOG_INFO, "[obs-airplay-cast] loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	StopAutoCastMonitor();
	if (g_airplay_cast)
		g_airplay_cast->Shutdown();
	g_auto_cast_timer.reset();
	g_airplay_cast.reset();
	blog(LOG_INFO, "[obs-airplay-cast] unloaded");
}
