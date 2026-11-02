#ifndef BTPT_SETTINGS_H
#define BTPT_SETTINGS_H

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include "capture.h"
#include "config.h"

class BluetoothPageTurner;
class QButtonGroup;
class QGridLayout;
class QHBoxLayout;
class QVBoxLayout;
class QLabel;
class QPushButton;

/* A full-screen page shown over Nickel for mapping a remote's buttons.
 *
 * Tap "Add" next to an action and press a button on the remote to map it.
 * Otherwise, pressing a button shows what it does.  Page turning is paused
 * while the screen is open. */
class SettingsScreen : public QWidget
{
	Q_OBJECT

public:
	SettingsScreen(BluetoothPageTurner *btpt, QWidget *parent,
	               const QString &uniq, bool newRemote);
	~SettingsScreen();

	void showRemote(const QString &uniq);

protected:
	bool eventFilter(QObject *obj, QEvent *event) override;

private:
	struct Action
	{
		QString method;
		QString label;
		QLabel *bindings;
		QPushButton *add;
		QPushButton *clear;
	};

	/* An event chosen from a press, kept so that a later capture can
	 * pick a different event if the two presses share it. */
	struct Capture
	{
		QList<RawEvent> burst;
		RawEvent chosen;
	};

	BluetoothPageTurner *btpt;

	QString uniq;
	QString name;
	QString path;
	bool exists = false;
	Config cfg;
	QMap<QString, QList<Capture> > captures;

	QList<Action> actions;
	QVBoxLayout *mainLayout;
	QGridLayout *grid;
	QHBoxLayout *debounceRow;
	QLabel *remoteLabel;
	QLabel *otherLabel;
	QLabel *status;
	QPushButton *nextRemote;
	QPushButton *ignore;
	QButtonGroup *debounceGroup;
	QList<int> debounceChoices;

	int capturing = -1;
	QList<RawEvent> burst;
	QTimer burstTimer;
	QTimer captureTimeout;
	QTimer flashTimer;

	void build();
	void applyScale();
	void loadRemote(const QString &uniq, const QString &name);
	void refresh();
	void save();
	void setStatus(const QString &text);
	void startCapture(int index);
	void stopCapture();
	void applyCapture();
	void showTest();
	QList<RawEvent> eventsNotFor(const QString &method) const;
	QString actionLabel(const QString &method) const;
	QString remoteTitle() const;

private slots:
	void onInput(const QString &uniq, const QString &name,
	             int type, int code, int value, int absMin, int absMax);
	void onRemotesChanged();
	void onBurstEnd();
	void onCaptureTimeout();
	void onFlashEnd();
	void onAdd();
	void onClear();
	void onDebounce(int id);
	void onNextRemote();
	void onIgnore();
};

/* Opens the settings screen, at most one at a time */
class SettingsLauncher : public QObject
{
	Q_OBJECT

public:
	explicit SettingsLauncher(BluetoothPageTurner *btpt);

public slots:
	void open(const QString &uniq);
	void offerSetup(const QString &uniq, const QString &name);

private:
	BluetoothPageTurner *btpt;
	QPointer<SettingsScreen> screen;
	void show(const QString &uniq, bool newRemote);
};

/* How a mapping is shown on screen: "KEY_VOLUMEUP", "ABS_Y 1012" */
QString describeEvent(int type, int code, int value);

#endif /* BTPT_SETTINGS_H */
