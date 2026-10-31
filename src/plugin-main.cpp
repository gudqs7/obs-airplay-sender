#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

#include <windows.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <plugin-support.h>

#include "airplay-cast.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

std::unique_ptr<AirPlayCast> g_airplay_cast;

std::wstring helper_path()
{
	HMODULE module = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&obs_module_load), &module);
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
	return (std::filesystem::path(path).parent_path() / L"obs-airplay-cast-bridge.exe")
		.wstring();
}

bool is_douyin_receiver_name(const std::string &name)
{
	constexpr const char *prefix = "抖音直播伴侣[";
	return name.rfind(prefix, 0) == 0 && !name.empty() && name.back() == ']';
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
		target->addItem(QString::fromUtf8(settings.target.c_str()),
				QString::fromUtf8(settings.target.c_str()));
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

	auto scanning = std::make_shared<std::atomic<bool>>(false);
	QPointer<QComboBox> target_guard(target);
	QPointer<QToolButton> scan_guard(scan_button);
	QPointer<QLabel> status_guard(scan_status);
	QPointer<QLabel> cast_status_guard(cast_status);
	QPointer<QPushButton> cast_guard(cast_button);
	const QString preferred = is_douyin_receiver_name(settings.target)
					  ? QString::fromUtf8(settings.target.c_str())
					  : QString();
	auto scan = [=]() {
		if (!g_airplay_cast || g_airplay_cast->State() != AirPlayCastState::Stopped)
			return;
		if (scanning->exchange(true))
			return;
		if (scan_guard)
			scan_guard->setEnabled(false);
		if (status_guard)
			status_guard->setText(obs_module_text("Settings.Scanning"));
		const auto scan_started = std::chrono::steady_clock::now();

		std::thread([=]() {
			std::vector<AirPlayDeviceInfo> devices;
			std::string error;
			const bool ok = g_airplay_cast &&
					g_airplay_cast->DiscoverDevices(
						devices, preferred.toUtf8().toStdString(), error);
			const auto elapsed_ms =
				std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - scan_started)
					.count();
			if (ok) {
				std::stable_sort(devices.begin(), devices.end(),
						 [](const AirPlayDeviceInfo &left,
						    const AirPlayDeviceInfo &right) {
							 const bool left_douyin =
								 is_douyin_receiver_name(left.name);
							 const bool right_douyin =
								 is_douyin_receiver_name(right.name);
							 if (left_douyin != right_douyin)
								 return left_douyin;
							 return left.name < right.name;
						 });
			}
			blog(LOG_INFO,
			     "[obs-airplay-cast] scan completed: devices=%zu elapsed=%lld ms%s%s",
			     devices.size(), static_cast<long long>(elapsed_ms),
			     ok ? "" : " error=", ok ? "" : error.c_str());

			QMetaObject::invokeMethod(
				qApp,
				[=]() {
					if (!target_guard)
						return;
					target_guard->clear();
					int select_index = -1;
					int preferred_index = -1;
					for (const AirPlayDeviceInfo &device : devices) {
						const QString name = QString::fromUtf8(device.name.c_str());
						const QString display =
							QStringLiteral("%1  (%2)")
								.arg(name, QString::fromUtf8(device.ip.c_str()));
						target_guard->addItem(display, name);
						target_guard->setItemData(
							target_guard->count() - 1,
							QString::fromUtf8(device.ip.c_str()),
							Qt::UserRole + 1);
						target_guard->setItemData(
							target_guard->count() - 1,
							static_cast<int>(device.port), Qt::UserRole + 2);
						const int item_index = target_guard->count() - 1;
						if (name == preferred)
							preferred_index = item_index;
						if (select_index < 0 &&
						    is_douyin_receiver_name(device.name))
							select_index = item_index;
					}
					target_guard->setCurrentIndex(
						preferred_index >= 0 ? preferred_index : select_index);

					if (status_guard) {
						if (!ok) {
							status_guard->setText(
								QString::fromUtf8(error.c_str()));
						} else if (devices.empty()) {
							status_guard->setText(
								obs_module_text("Settings.NoDevices"));
						} else {
							status_guard->setText(
								QString::fromUtf8(obs_module_text(
									"Settings.Found"))
									.arg(devices.size()));
						}
					}
					if (scan_guard)
						scan_guard->setEnabled(true);
					if (cast_guard)
						cast_guard->setEnabled(
							target_guard->currentIndex() >= 0);
					scanning->store(false);
				},
				Qt::QueuedConnection);
		}).detach();
	};
	QObject::connect(scan_button, &QToolButton::clicked, &dialog, scan);

	auto apply_settings = [&]() -> bool {
		settings.target = target->currentData().toString().toUtf8().toStdString();
		if (settings.target.empty())
			return false;
		settings.target_ip =
			target->currentData(Qt::UserRole + 1).toString().toUtf8().toStdString();
		settings.target_port =
			static_cast<uint16_t>(target->currentData(Qt::UserRole + 2).toInt());
		settings.fps = fps->currentData().toInt();
		settings.bitrate_kbps = bitrate->value();
		save_airplay_cast_settings(settings);
		return true;
	};

	auto set_inputs_enabled = [&](bool enabled) {
		target->setEnabled(enabled);
		scan_button->setEnabled(enabled);
		fps->setEnabled(enabled);
		bitrate->setEnabled(enabled);
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
			cast_status->setText(obs_module_text("Status.Idle"));
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
			QMessageBox::warning(&dialog, obs_module_text("Error.Title"),
					     QString::fromUtf8(error.c_str()));
		}
	};

	QObject::connect(cast_button, &QPushButton::clicked, &dialog, [&]() {
		if (!g_airplay_cast)
			return;
		if (g_airplay_cast->State() == AirPlayCastState::Running) {
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
		set_inputs_enabled(false);
		cast_button->setEnabled(false);
		cast_status->setText(obs_module_text("Status.Connecting"));
		g_airplay_cast->Start();
	});

	QObject::connect(close_button, &QPushButton::clicked, &dialog, [&]() {
		if (g_airplay_cast->State() == AirPlayCastState::Stopped && target->count() > 0)
			apply_settings();
		dialog.accept();
	});

	auto *state_timer = new QTimer(&dialog);
	state_timer->setInterval(250);
	QObject::connect(state_timer, &QTimer::timeout, &dialog, refresh_state);
	state_timer->start();

	if (g_airplay_cast && g_airplay_cast->State() == AirPlayCastState::Stopped) {
		scan();
	} else {
		scan_button->setEnabled(false);
		scan_status->setText(obs_module_text("Settings.StopBeforeScan"));
	}

	refresh_state();
	dialog.exec();
}

} // namespace

bool obs_module_load(void)
{
	register_airplay_cast_output();
	g_airplay_cast = std::make_unique<AirPlayCast>(helper_path());
	std::string warmup_error;
	if (!g_airplay_cast->WarmUp(warmup_error)) {
		blog(LOG_WARNING, "[obs-airplay-cast] bridge prewarm failed: %s",
		     warmup_error.c_str());
	}

	QAction *settings_action = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction(obs_module_text("Tools.Settings")));

	if (settings_action)
		QObject::connect(settings_action, &QAction::triggered, []() { show_settings_dialog(); });

	blog(LOG_INFO, "[obs-airplay-cast] loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	if (g_airplay_cast)
		g_airplay_cast->Shutdown();
	g_airplay_cast.reset();
	blog(LOG_INFO, "[obs-airplay-cast] unloaded");
}
