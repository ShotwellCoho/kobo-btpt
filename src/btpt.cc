#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <cstddef>
#include <cstdlib>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <linux/uinput.h>

#include <NickelHook.h>

#include <QApplication>
#include <QDir>
#include <QFileSystemWatcher>
#include <QFile>
#include <QWidget>
#include <QTextStream>
#include <QThread>

#include "btpt.h"
#include "capture.h"
#include "config.h"
#include "eventcodes.h"
#include "settings.h"

/* Stop Bluetooth heartbeat after this many seconds of inactivity */
#define BLUETOOTH_TIMEOUT (10*60)

static void *(*BluetoothHeartbeat)(void *, long long);
static void (*BluetoothHeartbeat_beat)(void *);

static void *(*MainWindowController_sharedInstance)();
static QWidget *(*MainWindowController_currentView)(void *);

static QObject *(*PowerManager_sharedInstance)();
static int (*PowerManager_filter)(QObject *, QObject *, QEvent *);
static QEvent::Type (*TimeEvent_eventType)();

static qint64 nowMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (qint64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static QWidget *currentView()
{
	void *mwc = MainWindowController_sharedInstance();
	if (!mwc) {
		nh_log("invalid MainWindowController");
		return NULL;
	}
	QWidget *cv = MainWindowController_currentView(mwc);
	if (!cv) {
		nh_log("invalid View");
	}
	return cv;
}

QWidget *nickelMainWindow()
{
	QWidget *cv = currentView();
	if (cv) {
		return cv->window();
	}
	return QApplication::activeWindow();
}

static void invokeMainWindowController(const char *method)
{
	QWidget *cv = currentView();
	if (!cv) {
		return;
	}
	QString name = cv->objectName();
	if (name == "ReadingView") {
		nh_log("%s", method);
		if (!QMetaObject::invokeMethod(cv, method, Qt::QueuedConnection)) {
			nh_log("ReadingView has no invokable method %s", method);
		}
	}
	else {
		nh_log("not reading view");
	}
}

BluetoothPageTurner::BluetoothPageTurner()
	: settingsOpen(false)
{
	/* These live in the GUI thread, and their slots run there */
	QObject::connect(
		&inputWatcher, &QFileSystemWatcher::directoryChanged,
		this, &BluetoothPageTurner::inputDirectoryChanged);
	inputWatcher.addPath("/dev/input");

	QObject::connect(
		&configWatcher, &QFileSystemWatcher::directoryChanged,
		this, &BluetoothPageTurner::configChangedOnDisk);
	QObject::connect(
		&configWatcher, &QFileSystemWatcher::fileChanged,
		this, &BluetoothPageTurner::configChangedOnDisk);

	/* Watches can be lost, for example when the onboard storage is
	 * unmounted for USB, so renew them now and then */
	QObject::connect(
		&rewatchTimer, &QTimer::timeout,
		this, &BluetoothPageTurner::watchConfig);
	rewatchTimer.start(30 * 1000);
	watchConfig();
}

QList<RemoteInfo> BluetoothPageTurner::remotes()
{
	QMutexLocker locker(&mutex);
	return remoteList;
}

void BluetoothPageTurner::setSettingsOpen(bool open)
{
	settingsOpen = open;
}

void BluetoothPageTurner::requestReload()
{
	QMutexLocker locker(&mutex);
	configChanged = true;
	wake.wakeAll();
}

void BluetoothPageTurner::inputDirectoryChanged(const QString &path)
{
	(void)path;
	QMutexLocker locker(&mutex);
	deviceChanges++;
	wake.wakeAll();
}

void BluetoothPageTurner::configChangedOnDisk(const QString &path)
{
	(void)path;
	watchConfig();
	requestReload();
}

void BluetoothPageTurner::watchConfig()
{
	if (QFile::exists(BTPT_OPEN_SETTINGS)) {
		QFile::remove(BTPT_OPEN_SETTINGS);
		emit openSettings(QString());
	}

	if (!configWatcher.directories().contains(BTPT_DIR) &&
	    QDir(BTPT_DIR).exists()) {
		configWatcher.addPath(BTPT_DIR);
	}

	/* Directory watches only see files being added and removed, so watch
	 * each file for edits too.  Saving replaces a file, which drops its
	 * watch, so this runs again after every change. */
	QStringList files;
	foreach (const QString &f, QDir(BTPT_DIR).entryList(QDir::Files)) {
		QString path = BTPT_DIR + f;
		if (!configWatcher.files().contains(path)) {
			files << path;
		}
	}
	if (!files.isEmpty()) {
		configWatcher.addPaths(files);
	}
}

void BluetoothPageTurner::loadConfig(Device &device)
{
	bool exists;
	QString path = configPath(device.name, device.uniq, &exists);
	device.hasConfig = exists && device.cfg.load(path);
	if (!device.hasConfig) {
		device.cfg = Config();
	}
}

void BluetoothPageTurner::addDevice(
	const QString &name, const QString &uniq, const QString &handler)
{
	if (devices.contains(handler)) {
		return;
	}

	QString path = "/dev/input/" + handler;
	int fd = open(path.toLatin1().constData(), O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		nh_log("error opening %s", qPrintable(path));
		return;
	}

	/* Open every Bluetooth input device, even without a configuration,
	 * so that the settings screen can set it up. */
	Device &device = devices[handler];
	device.fd = fd;
	device.handler = handler;
	device.uniq = uniq;
	device.name = name;
	loadConfig(device);

	nh_log("acquired device %s: %s (%s)%s",
	       qPrintable(handler), qPrintable(uniq), qPrintable(name),
	       device.hasConfig ? "" : ", not configured");
}

void BluetoothPageTurner::scanDevices()
{
	QFile file("/proc/bus/input/devices");
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		nh_log("error opening /proc/bus/input/devices");
		return;
	}

	nh_log("checking /proc/bus/input/devices for new devices");

	QByteArray text = file.readAll();

	file.close();

	QTextStream in(&text);
	QString i;
	QString u;
	QString n;
	while (!in.atEnd()) {
		QString line = in.readLine();
		QString type = line.section(": ", 0, 0);
		if (type == "I") {
			i = line.section(": ", 1);
			u.clear();
			n.clear();
		}
		else if (type == "U") {
			/* Bluetooth address without the ':'s */
			u = line.section(": Uniq=", 1);
			u.remove(':');
		}
		else if (type == "N") {
			/* Bluetooth name without surrounding double-quotes */
			n = line.section(": Name=", 1);
			n.chop(1);
			n.remove(0, 1);
		}
		else if (type == "H") {
			/* Skip if not Bluetooth device */
			if (!i.startsWith("Bus=0005 ")) {
				continue;
			}

			QList<QString> handlers =
				line.section("Handlers=", 1).split(" ");
			foreach (const QString &handler, handlers) {
				if (handler.startsWith("event")) {
					addDevice(n, u, handler);
				}
			}
		}
	}

	nh_log("devices scanned");
}

void BluetoothPageTurner::publishRemotes()
{
	QList<RemoteInfo> list;
	QSet<QString> seen;
	foreach (const Device &device, devices) {
		QString key = device.uniq.isEmpty() ? device.name : device.uniq;
		if (seen.contains(key)) {
			continue;
		}
		seen.insert(key);
		RemoteInfo info;
		info.uniq = device.uniq;
		info.name = device.name;
		list.append(info);
	}

	mutex.lock();
	remoteList = list;
	mutex.unlock();

	emit remotesChanged();
}

void BluetoothPageTurner::handleEvent(Device &device, const struct input_event &e)
{
	if (settingsOpen) {
		if (e.type == EV_SYN) {
			return;
		}
		int absMin = 0, absMax = 0;
		if (e.type == EV_ABS) {
			struct input_absinfo info;
			if (ioctl(device.fd, EVIOCGABS(e.code), &info) == 0) {
				absMin = info.minimum;
				absMax = info.maximum;
			}
		}
		emit inputEvent(device.uniq, device.name,
		                e.type, e.code, e.value, absMin, absMax);
		return;
	}

	if (!device.hasConfig) {
		/* Offer to set up a new remote the first time it is used */
		QString key = device.uniq.isEmpty() ? device.name : device.uniq;
		if (!isNoise(e.type) && !prompted.contains(key)) {
			prompted.insert(key);
			nh_log("unconfigured input from %s", qPrintable(key));
			emit unconfiguredInput(device.uniq, device.name);
		}
		return;
	}

	foreach (const Mapping &m, device.cfg.mappings) {
		if (!m.matches(e.type, e.code, e.value)) {
			continue;
		}

		/* Debounce per remote, across all of its handlers and
		 * buttons, so one press can't turn two pages */
		qint64 now = nowMs();
		QString key = device.uniq.isEmpty() ? device.name : device.uniq;
		int debounce = device.cfg.debounceMs;
		if (debounce > 0 && lastFire.contains(key) &&
		    now - lastFire[key] < debounce) {
			nh_log("debounced %s", qPrintable(m.method));
			continue;
		}
		lastFire[key] = now;

		/* Update Bluetooth heartbeat time */
		lastEvent = now;

		/* Update PowerManager::timeLastUsed */
		emit notify();

		/* Invoke the configured method */
		QByteArray method = m.method.toLatin1();
		invokeMainWindowController(method.constData());
	}
}

void BluetoothPageTurner::run()
{
	void *bluetoothHeartbeat = NULL;
	fd_set rfds;
	struct timeval tv;

	nh_log("starting");

	/* Pick up devices that connected before we started */
	mutex.lock();
	deviceChanges++;
	mutex.unlock();

	while (1) {
		struct input_event e;
		int ret;

		/* Wait for something to do if there are no devices */
		mutex.lock();
		if (deviceChanges == 0 && !configChanged && devices.empty()) {
			nh_log("waiting for input devices");
			wake.wait(&mutex);
		}
		bool rescan = deviceChanges > 0;
		bool reload = configChanged;
		deviceChanges = 0;
		configChanged = false;
		mutex.unlock();

		if (rescan) {
			scanDevices();
			publishRemotes();
		}
		if (reload) {
			nh_log("reloading configuration");
			for (auto it = devices.begin(); it != devices.end(); ++it) {
				loadConfig(it.value());
			}
		}

		/* Bluetooth heartbeat to prevent Bluetooth from turning off */
		if (lastEvent > 0 &&
		    nowMs() - lastEvent < BLUETOOTH_TIMEOUT * 1000) {
			if (bluetoothHeartbeat == NULL) {
				bluetoothHeartbeat = alloca(1024);
				new(bluetoothHeartbeat) QObject();
				BluetoothHeartbeat(bluetoothHeartbeat, 0);
			}
			BluetoothHeartbeat_beat(bluetoothHeartbeat);
		}

		if (devices.empty()) {
			continue;
		}

		/* Poll devices for input events */
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&rfds);

		foreach (const Device &device, devices) {
			FD_SET(device.fd, &rfds);
		}

		ret = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
		if (ret <= 0) {
			continue;
		}

		/* Process events for each device */
		bool lostAny = false;
		for (auto it = devices.begin(); it != devices.end();) {
			Device &device = it.value();

			if (!FD_ISSET(device.fd, &rfds)) {
				it++;
				continue;
			}

			/* Drain everything queued, so a burst of events from
			 * one press is handled together */
			bool lost = false;
			while (1) {
				ret = read(device.fd, &e, sizeof(e));
				if (ret < 0 && errno == EAGAIN) {
					break;
				}
				if (ret < (int)sizeof(e)) {
					lost = true;
					break;
				}
				if (e.type == 0x00 &&
				    e.code == 0x00 &&
				    e.value == 0x0001) {
					lost = true;
					break;
				}
				handleEvent(device, e);
			}

			if (lost) {
				nh_log("lost device %s", qPrintable(device.handler));
				close(device.fd);
				it = devices.erase(it);
				lostAny = true;
				continue;
			}

			it++;
		}

		if (lostAny) {
			publishRemotes();
		}
	}
}

void TimeLastUsedUpdater::notify()
{
	/* Update PowerManager::timeLastUsed to prevent sleep */
	QEvent timeEvent(TimeEvent_eventType());

	QObject *pm = PowerManager_sharedInstance();
	if (pm == NULL) {
		nh_log("invalid PowerManager");
		return;
	}

	PowerManager_filter(pm, this, &timeEvent);
}

static int btpt_init()
{
	mkdir(BTPT_DIR, 0755);
	TimeLastUsedUpdater *timeLastUsedUpdater = new TimeLastUsedUpdater();
	BluetoothPageTurner *btpt = new BluetoothPageTurner();
	QObject::connect(
		btpt, &BluetoothPageTurner::notify,
		timeLastUsedUpdater, &TimeLastUsedUpdater::notify,
		Qt::QueuedConnection);
	QObject::connect(
		btpt, &QThread::finished,
		btpt, &QObject::deleteLater);
	QObject::connect(
		btpt, &QThread::finished,
		timeLastUsedUpdater, &QObject::deleteLater);

	SettingsLauncher *launcher = new SettingsLauncher(btpt);
	QObject::connect(
		btpt, &BluetoothPageTurner::openSettings,
		launcher, &SettingsLauncher::open,
		Qt::QueuedConnection);
	QObject::connect(
		btpt, &BluetoothPageTurner::unconfiguredInput,
		launcher, &SettingsLauncher::offerSetup,
		Qt::QueuedConnection);
	QObject::connect(
		btpt, &QThread::finished,
		launcher, &QObject::deleteLater);

	btpt->start();

	return 0;
}

static struct nh_info btpt_info = {
	.name           = "BluetoothPageTurner",
	.desc           = "Turn pages with Bluetooth device",
	.uninstall_flag = BTPT_DIR "uninstall",
};

static struct nh_hook btpt_hook[] = {
	{0},
};

static struct nh_dlsym btpt_dlsym[] = {
	{
		.name = "_ZN18BluetoothHeartbeatC1Ex",
		.out = nh_symoutptr(BluetoothHeartbeat)
	},
	{
		.name = "_ZN18BluetoothHeartbeat4beatEv",
		.out = nh_symoutptr(BluetoothHeartbeat_beat)
	},
	{
		.name = "_ZN20MainWindowController14sharedInstanceEv",
		.out = nh_symoutptr(MainWindowController_sharedInstance)
	},
	{
		.name = "_ZNK20MainWindowController11currentViewEv",
		.out = nh_symoutptr(MainWindowController_currentView)
	},
	{
		.name = "_ZN9TimeEvent9eventTypeEv",
		.out = nh_symoutptr(TimeEvent_eventType)
	},
	{
		.name = "_ZN12PowerManager14sharedInstanceEv",
		.out = nh_symoutptr(PowerManager_sharedInstance)
	},
	{
		.name = "_ZN12PowerManager6filterEP7QObjectP6QEvent",
		.out = nh_symoutptr(PowerManager_filter)
	},
	{0},
};

NickelHook(
	.init  = btpt_init,
	.info  = &btpt_info,
	.hook  = btpt_hook,
	.dlsym = btpt_dlsym,
)
