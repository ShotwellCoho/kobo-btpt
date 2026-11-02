#ifndef BTPT_H
#define BTPT_H

#include <linux/input.h>
#include <linux/uinput.h>

#include <atomic>

#include <QFileSystemWatcher>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QPair>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include "config.h"

/* Creating this file (for example from a NickelMenu entry) opens the
 * settings screen */
#define BTPT_OPEN_SETTINGS BTPT_DIR "open-settings"

/* One /dev/input handler of a Bluetooth device.  A device can have several
 * (for example a keyboard and a mouse), which share a configuration. */
class Device
{
public:
	int fd = -1;
	QString handler;
	QString uniq;
	QString name;
	Config cfg;
	bool hasConfig = false;
};

struct RemoteInfo
{
	QString uniq;
	QString name;
};

class BluetoothPageTurner : public QThread
{
	Q_OBJECT

public:
	BluetoothPageTurner();

	/* Connected Bluetooth input devices; safe to call from any thread */
	QList<RemoteInfo> remotes();

	/* While the settings screen is open, input is sent to it with
	 * inputEvent() instead of turning pages. */
	void setSettingsOpen(bool open);

	/* Re-read configuration files */
	void requestReload();

private:
	void run() override;
	void scanDevices();
	void addDevice(const QString &name, const QString &uniq,
	               const QString &handler);
	void loadConfig(Device &device);
	void handleEvent(Device &device, const struct input_event &e);
	void publishRemotes();

	/* Owned by the reader thread */
	QMap<QString, Device> devices; /* by handler */
	QMap<QString, qint64> lastFire; /* by uniq, for debouncing */
	QSet<QString> prompted;
	qint64 lastEvent = 0;

	/* Shared, protected by mutex */
	QMutex mutex;
	QWaitCondition wake;
	int deviceChanges = 0;
	bool configChanged = false;
	QList<RemoteInfo> remoteList;

	std::atomic<bool> settingsOpen;

	/* Owned by the GUI thread */
	QFileSystemWatcher inputWatcher;
	QFileSystemWatcher configWatcher;
	QTimer rewatchTimer;

public slots:
	void inputDirectoryChanged(const QString &path);
	void configChangedOnDisk(const QString &path);
	void watchConfig();

signals:
	void notify();
	void deviceAction(const QString &method);
	void openSettings(const QString &uniq);
	void inputEvent(const QString &uniq, const QString &name,
	                int type, int code, int value,
	                int absMin, int absMax);
	void unconfiguredInput(const QString &uniq, const QString &name);
	void remotesChanged();
};

class TimeLastUsedUpdater : public QObject
{
	Q_OBJECT

public slots:
	void notify();
};

/* Nickel's main window, to show the settings screen over */
QWidget *nickelMainWindow();

/* The view Nickel is showing, such as ReadingView */
QWidget *nickelCurrentView();

#endif /* BTPT_H */
