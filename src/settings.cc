#include <QButtonGroup>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>

#include <stdlib.h>

#include <linux/input-event-codes.h>

#include <NickelHook.h>

#include "actions.h"
#include "btpt.h"
#include "eventcodes.h"
#include "settings.h"

/* Debounce for remotes first set up on this screen */
#define DEFAULT_DEBOUNCE_MS 250

/* A press is over once the remote has been quiet for this long */
#define BURST_MS 400

QString describeEvent(int type, int code, int value)
{
	QString name = eventCodeName(type, code);
	if (type == EV_KEY) {
		return value == 0 ? name + " (release)" : name;
	}
	return name + ' ' + QString::number(value);
}

static RawEvent toRaw(const Mapping &m)
{
	RawEvent e;
	e.type = m.type;
	e.code = m.code;
	e.value = m.value;
	e.absMin = 0;
	e.absMax = 0;
	return e;
}

/* Keys going up and axes returning to the middle: what a remote sends
 * when a held button is let go */
static bool onlyReleases(const QList<RawEvent> &burst)
{
	bool any = false;
	foreach (const RawEvent &e, burst) {
		if (isNoise(e.type)) {
			continue;
		}
		any = true;
		if (e.type == EV_KEY && e.value == 0) {
			continue;
		}
		if (e.type == EV_ABS && e.absMax > e.absMin) {
			long range = (long)e.absMax - e.absMin;
			long off = labs(2L * e.value - ((long)e.absMax + e.absMin));
			if (off * 10 <= range) {
				continue;
			}
		}
		if (e.type == EV_REL && e.value == 0) {
			continue;
		}
		return false;
	}
	return any;
}

static QFrame *rule(QWidget *parent)
{
	QFrame *line = new QFrame(parent);
	line->setObjectName("rule");
	line->setFixedHeight(2);
	return line;
}

SettingsScreen::SettingsScreen(BluetoothPageTurner *btpt, QWidget *parent,
                               const QString &uniq, bool newRemote)
	: QWidget(parent), btpt(btpt)
{
	setObjectName("btptSettings");
	setAttribute(Qt::WA_StyledBackground, true);
	setAutoFillBackground(true);

	build();

	burstTimer.setSingleShot(true);
	burstTimer.setInterval(BURST_MS);
	connect(&burstTimer, &QTimer::timeout, this, &SettingsScreen::onBurstEnd);
	captureTimeout.setSingleShot(true);
	captureTimeout.setInterval(30 * 1000);
	connect(&captureTimeout, &QTimer::timeout,
	        this, &SettingsScreen::onCaptureTimeout);
	flashTimer.setSingleShot(true);
	flashTimer.setInterval(1500);
	connect(&flashTimer, &QTimer::timeout, this, &SettingsScreen::onFlashEnd);

	connect(btpt, &BluetoothPageTurner::inputEvent,
	        this, &SettingsScreen::onInput, Qt::QueuedConnection);
	connect(btpt, &BluetoothPageTurner::remotesChanged,
	        this, &SettingsScreen::onRemotesChanged, Qt::QueuedConnection);
	btpt->setSettingsOpen(true);

	if (parent) {
		parent->installEventFilter(this);
		setGeometry(parent->rect());
	}
	applyScale();

	showRemote(uniq);

	if (uniq.isEmpty() && name.isEmpty()) {
		setStatus("Connect your remote in Kobo's Bluetooth settings, "
		          "then press one of its buttons.");
	}
	else if (newRemote) {
		setStatus("New remote. Tap Add next to an action, then press "
		          "the button you want for it.");
	}
	else {
		setStatus("Press a button on your remote to see what it does, "
		          "or tap Add to map one.");
	}

	nh_log("settings opened");
}

SettingsScreen::~SettingsScreen()
{
	btpt->setSettingsOpen(false);
	nh_log("settings closed");
}

/* Size everything from the screen, so it suits each Kobo's resolution, is
 * easy to tap, and fits in landscape too */
void SettingsScreen::applyScale()
{
	int unit = qMax(16, qMin(width() / 30, height() / 42));
	int small = unit * 3 / 4;
	int pad = unit / 2;

	/* E-ink: black on white, thick borders, inverted when pressed or
	 * selected, no grey.  Everything is set explicitly so that Nickel's
	 * own style sheets don't leak in. */
	setStyleSheet(QString(
		"#btptSettings, #btptSettings QWidget {"
		"  background: white; color: black; font-size: %1px;"
		"  font-weight: normal; margin: 0; padding: 0; border: none; }"
		"#btptSettings #title { font-size: %2px; font-weight: bold; }"
		"#btptSettings #action { font-weight: bold; }"
		"#btptSettings #small { font-size: %3px; }"
		"#btptSettings #status { border: 2px solid black; padding: %4px; }"
		"#btptSettings #rule { background: black; }"
		"#btptSettings QLabel[flash=\"true\"]"
		"  { background: black; color: white; }"
		"#btptSettings QPushButton { border: 3px solid black;"
		"  border-radius: %5px; padding: %5px %4px; min-height: %6px; }"
		"#btptSettings QPushButton:pressed,"
		"#btptSettings QPushButton:checked"
		"  { background: black; color: white; }"
		"#btptSettings QPushButton:disabled { border-style: dotted; }")
		.arg(unit).arg(unit * 4 / 3).arg(small)
		.arg(pad).arg(pad / 2).arg(unit * 3 / 2));

	mainLayout->setContentsMargins(unit, unit, unit, unit);
	mainLayout->setSpacing(pad);
	grid->setHorizontalSpacing(pad);
	grid->setVerticalSpacing(pad);
	debounceRow->setSpacing(pad);
	status->setMinimumHeight(unit * 3);
}

void SettingsScreen::build()
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	mainLayout = layout;

	QHBoxLayout *header = new QHBoxLayout();
	QLabel *title = new QLabel("Page turner", this);
	title->setObjectName("title");
	header->addWidget(title, 1);
	QPushButton *close = new QPushButton("Close", this);
	connect(close, &QPushButton::clicked, this, &QObject::deleteLater);
	header->addWidget(close);
	layout->addLayout(header);

	QHBoxLayout *remote = new QHBoxLayout();
	remoteLabel = new QLabel(this);
	remoteLabel->setWordWrap(true);
	remote->addWidget(remoteLabel, 1);
	nextRemote = new QPushButton("Next remote", this);
	connect(nextRemote, &QPushButton::clicked,
	        this, &SettingsScreen::onNextRemote);
	remote->addWidget(nextRemote);
	layout->addLayout(remote);

	layout->addWidget(rule(this));

	QStringList methods, labels;
	methods << "nextPage" << "prevPage";
	labels << "Next page" << "Previous page";
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	methods << "goToNextChapter" << "goToPrevChapter";
#else
	methods << "nextChapter" << "prevChapter";
#endif
	labels << "Next chapter" << "Previous chapter";
	/* Device actions, which work in any view */
	int firstDeviceAction = methods.size();
	methods << "nickel:sleep" << "nickel:nightMode" << "nickel:rotate";
	labels << "Sleep" << "Night mode" << "Rotate screen";

	grid = new QGridLayout();
	grid->setColumnStretch(0, 1);
	int row = 0;
	for (int i = 0; i < methods.size(); i++) {
		if (i == firstDeviceAction) {
			QLabel *heading = new QLabel("Anywhere, not only in books", this);
			heading->setObjectName("small");
			grid->addWidget(heading, row++, 0, 1, 3);
		}

		Action a;
		a.method = methods[i];
		a.label = labels[i];

		QLabel *label = new QLabel(a.label, this);
		label->setObjectName("action");
		a.bindings = new QLabel(this);
		a.bindings->setObjectName("small");
		a.bindings->setWordWrap(true);

		QVBoxLayout *text = new QVBoxLayout();
		text->setSpacing(0);
		text->addWidget(label);
		text->addWidget(a.bindings);
		grid->addLayout(text, row, 0);

		a.add = new QPushButton("Add", this);
		a.add->setProperty("action", i);
		connect(a.add, &QPushButton::clicked, this, &SettingsScreen::onAdd);
		grid->addWidget(a.add, row, 1);

		a.clear = new QPushButton("Clear", this);
		a.clear->setProperty("action", i);
		connect(a.clear, &QPushButton::clicked,
		        this, &SettingsScreen::onClear);
		grid->addWidget(a.clear, row, 2);
		row++;

		actions.append(a);
	}
	layout->addLayout(grid);

	otherLabel = new QLabel(this);
	otherLabel->setObjectName("small");
	otherLabel->setWordWrap(true);
	layout->addWidget(otherLabel);

	layout->addWidget(rule(this));

	QLabel *debounceLabel = new QLabel("Ignore repeat presses within", this);
	debounceLabel->setObjectName("action");
	layout->addWidget(debounceLabel);
	QLabel *debounceHelp = new QLabel(
		"Stops one press turning two pages. Raise it if pages skip; "
		"lower it if quick presses get missed.", this);
	debounceHelp->setObjectName("small");
	debounceHelp->setWordWrap(true);
	layout->addWidget(debounceHelp);

	QHBoxLayout *debounce = new QHBoxLayout();
	debounceRow = debounce;
	debounceGroup = new QButtonGroup(this);
	debounceGroup->setExclusive(true);
	debounceChoices << 0 << 150 << 250 << 500 << 1000;
	foreach (int ms, debounceChoices) {
		QPushButton *b = new QPushButton(
			ms == 0 ? QString("Off") : QString("%1 ms").arg(ms), this);
		b->setCheckable(true);
		connect(b, &QPushButton::clicked, this, [this, ms]() {
			onDebounce(ms);
		});
		debounceGroup->addButton(b, ms);
		debounce->addWidget(b);
	}
	debounce->addStretch(1);
	layout->addLayout(debounce);

	layout->addWidget(rule(this));

	status = new QLabel(this);
	status->setObjectName("status");
	status->setWordWrap(true);
	layout->addWidget(status);

	layout->addStretch(1);

	ignore = new QPushButton("This isn't a page turner", this);
	connect(ignore, &QPushButton::clicked, this, &SettingsScreen::onIgnore);
	layout->addWidget(ignore, 0, Qt::AlignLeft);
}

QString SettingsScreen::remoteTitle() const
{
	if (uniq.isEmpty() && name.isEmpty()) {
		return "No remote connected";
	}
	return name.isEmpty() ? uniq : name;
}

void SettingsScreen::showRemote(const QString &wanted)
{
	QList<RemoteInfo> remotes = btpt->remotes();
	foreach (const RemoteInfo &r, remotes) {
		if (!wanted.isEmpty() && (r.uniq == wanted || r.name == wanted)) {
			loadRemote(r.uniq, r.name);
			return;
		}
	}
	if (!remotes.isEmpty()) {
		loadRemote(remotes.first().uniq, remotes.first().name);
	}
	else {
		loadRemote(QString(), QString());
	}
}

void SettingsScreen::loadRemote(const QString &newUniq, const QString &newName)
{
	if (newUniq != uniq || newName != name) {
		captures.clear();
	}
	uniq = newUniq;
	name = newName;
	if (uniq.isEmpty() && name.isEmpty()) {
		path.clear();
		exists = false;
		cfg = Config();
	}
	else {
		path = configPath(name, uniq, &exists);
		if (!exists) {
			/* What a new file starts as once something is saved */
			cfg.parse(QString(
				"# %1, set up with the page turner settings screen.\n"
				"debounce %2\n").arg(remoteTitle())
				.arg(DEFAULT_DEBOUNCE_MS).toUtf8());
		}
		else {
			cfg.load(path);
		}
	}
	refresh();
}

void SettingsScreen::refresh()
{
	bool haveRemote = !path.isEmpty();
	QList<RemoteInfo> remotes = btpt->remotes();

	bool connected = false;
	foreach (const RemoteInfo &r, remotes) {
		if (r.uniq == uniq && r.name == name) {
			connected = true;
		}
	}
	QString text = "<b>" + remoteTitle().toHtmlEscaped() + "</b>";
	if (haveRemote && !connected) {
		text += " (disconnected)";
	}
	if (haveRemote) {
		text += "<br><small>" +
			QString(exists ? path : "Not set up yet").toHtmlEscaped() +
			"</small>";
	}
	remoteLabel->setText(text);
	nextRemote->setVisible(remotes.size() > 1);

	QStringList known;
	for (int i = 0; i < actions.size(); i++) {
		Action &a = actions[i];
		known << a.method;
		QStringList parts;
		foreach (const Mapping &m, cfg.mappingsFor(a.method)) {
			parts << describeEvent(m.type, m.code, m.value);
		}
		if (capturing == i) {
			a.bindings->setText("Press the button now…");
		}
		else {
			a.bindings->setText(parts.isEmpty() ?
				QString("Not mapped") : parts.join(", "));
		}
		a.add->setText(capturing == i ? "Cancel" : "Add");
		a.add->setEnabled(haveRemote);
		a.clear->setEnabled(haveRemote && !parts.isEmpty());
	}

	/* Mappings for methods this screen doesn't list are kept as they are */
	QStringList other;
	foreach (const Mapping &m, cfg.mappings) {
		if (!known.contains(m.method)) {
			other << m.method + ": " +
				describeEvent(m.type, m.code, m.value);
		}
	}
	otherLabel->setVisible(!other.isEmpty());
	otherLabel->setText("Also in the file: " + other.join(", "));

	QAbstractButton *b = debounceGroup->button(cfg.debounceMs);
	if (b) {
		b->setChecked(true);
	}
	else {
		/* A custom value from the file: show no choice as selected */
		debounceGroup->setExclusive(false);
		foreach (QAbstractButton *button, debounceGroup->buttons()) {
			button->setChecked(false);
		}
		debounceGroup->setExclusive(true);
	}
	foreach (QAbstractButton *button, debounceGroup->buttons()) {
		button->setEnabled(haveRemote);
	}

	ignore->setVisible(haveRemote && !exists);
}

void SettingsScreen::save()
{
	if (path.isEmpty()) {
		return;
	}
	if (cfg.save(path)) {
		exists = true;
		btpt->requestReload();
	}
	else {
		setStatus("Couldn't save " + path + ".");
	}
}

void SettingsScreen::setStatus(const QString &text)
{
	status->setText(text);
}

QString SettingsScreen::actionLabel(const QString &method) const
{
	foreach (const Action &a, actions) {
		if (a.method == method) {
			return a.label;
		}
	}
	return method;
}

/* Everything the remote sends for other actions: their mapped events and
 * every event seen while capturing them. */
QList<RawEvent> SettingsScreen::eventsNotFor(const QString &method) const
{
	QList<RawEvent> list;
	foreach (const Mapping &m, cfg.mappings) {
		if (m.method != method) {
			list << toRaw(m);
		}
	}
	for (QMap<QString, QList<Capture> >::const_iterator it =
	         captures.constBegin(); it != captures.constEnd(); ++it) {
		if (it.key() == method) {
			continue;
		}
		foreach (const Capture &c, it.value()) {
			list << c.burst;
		}
	}
	return list;
}

void SettingsScreen::startCapture(int index)
{
	capturing = index;
	burst.clear();
	burstTimer.stop();
	captureTimeout.start();
	setStatus("Press the button for " + actions[index].label + " on " +
	          remoteTitle() + ".");
	refresh();
}

void SettingsScreen::stopCapture()
{
	capturing = -1;
	captureTimeout.stop();
	refresh();
}

void SettingsScreen::applyCapture()
{
	const Action &a = actions[capturing];

	int index = pickEvent(burst, eventsNotFor(a.method));
	if (index < 0) {
		setStatus("That didn't look like a button press. Try again, "
		          "or tap Cancel.");
		captureTimeout.start();
		return;
	}
	RawEvent chosen = burst[index];

	/* One button, one action */
	cfg.removeEvent(chosen.type, chosen.code, chosen.value);
	Mapping m;
	m.method = a.method;
	m.type = chosen.type;
	m.code = chosen.code;
	m.value = chosen.value;
	cfg.addMapping(m);

	Capture capture;
	capture.burst = burst;
	capture.chosen = chosen;
	captures[a.method].append(capture);

	QString message = a.label + ": " +
		describeEvent(chosen.type, chosen.code, chosen.value) + ".";

	/* A button picked earlier may send the same event as this one (two
	 * swipes on a ring both start with a touch).  Pick again for it,
	 * avoiding everything this press sent. */
	for (QMap<QString, QList<Capture> >::iterator it = captures.begin();
	     it != captures.end(); ++it) {
		if (it.key() == a.method) {
			continue;
		}
		for (int i = 0; i < it.value().size(); i++) {
			Capture &c = it.value()[i];
			bool clash = false;
			foreach (const RawEvent &e, burst) {
				if (e.same(c.chosen)) {
					clash = true;
				}
			}
			if (!clash) {
				continue;
			}
			int j = pickEvent(c.burst, eventsNotFor(it.key()));
			if (j < 0 || c.burst[j].same(c.chosen)) {
				continue;
			}
			RawEvent old = c.chosen;
			c.chosen = c.burst[j];
			cfg.removeEvent(old.type, old.code, old.value);
			cfg.removeEvent(c.chosen.type, c.chosen.code, c.chosen.value);
			Mapping n;
			n.method = it.key();
			n.type = c.chosen.type;
			n.code = c.chosen.code;
			n.value = c.chosen.value;
			cfg.addMapping(n);
			message += " Changed " + actionLabel(it.key()) + " to " +
				describeEvent(n.type, n.code, n.value) +
				" so they don't clash.";
		}
	}

	save();
	stopCapture();
	setStatus(message + " Press it again to try it.");
	nh_log("mapped %s", qPrintable(message));
}

void SettingsScreen::showTest()
{
	foreach (const Action &a, actions) {
		a.bindings->setProperty("flash", false);
	}

	QStringList matched;
	QStringList seen;
	foreach (const RawEvent &e, burst) {
		if (isNoise(e.type)) {
			continue;
		}
		foreach (const Mapping &m, cfg.mappings) {
			if (m.matches(e.type, e.code, e.value)) {
				QString label = actionLabel(m.method);
				if (!matched.contains(label)) {
					matched << label;
				}
				for (int i = 0; i < actions.size(); i++) {
					if (actions[i].method == m.method) {
						actions[i].bindings->setProperty("flash", true);
					}
				}
			}
		}
		QString d = describeEvent(e.type, e.code, e.value);
		if (!seen.contains(d) && seen.size() < 6) {
			seen << d;
		}
	}

	if (matched.isEmpty() && onlyReleases(burst)) {
		/* The end of a press that was held down; nothing to say */
		return;
	}
	if (matched.isEmpty()) {
		setStatus("That button isn't mapped. It sent: " +
		          seen.join(", ") + ".");
	}
	else {
		setStatus("That button does: " + matched.join(", ") + ".");
	}

	foreach (const Action &a, actions) {
		a.bindings->style()->unpolish(a.bindings);
		a.bindings->style()->polish(a.bindings);
	}
	flashTimer.start();
}

void SettingsScreen::onInput(const QString &fromUniq, const QString &fromName,
                             int type, int code, int value,
                             int absMin, int absMax)
{
	/* Pressing a button on another remote switches to it */
	if (fromUniq != uniq || fromName != name) {
		if (!burst.isEmpty()) {
			return; /* finish the current press first */
		}
		loadRemote(fromUniq, fromName);
	}

	RawEvent e;
	e.type = type;
	e.code = code;
	e.value = value;
	e.absMin = absMin;
	e.absMax = absMax;
	burst.append(e);
	burstTimer.start();
}

void SettingsScreen::onBurstEnd()
{
	if (capturing >= 0) {
		applyCapture();
	}
	else {
		showTest();
	}
	burst.clear();
}

void SettingsScreen::onCaptureTimeout()
{
	if (capturing >= 0) {
		stopCapture();
		setStatus("No button pressed. Tap Add to try again.");
	}
}

void SettingsScreen::onFlashEnd()
{
	foreach (const Action &a, actions) {
		a.bindings->setProperty("flash", false);
		a.bindings->style()->unpolish(a.bindings);
		a.bindings->style()->polish(a.bindings);
	}
}

void SettingsScreen::onAdd()
{
	int index = sender()->property("action").toInt();
	if (capturing == index) {
		stopCapture();
		setStatus("Cancelled.");
	}
	else {
		startCapture(index);
	}
}

void SettingsScreen::onClear()
{
	int index = sender()->property("action").toInt();
	const Action &a = actions[index];
	if (capturing == index) {
		stopCapture();
	}
	cfg.removeMethod(a.method);
	captures.remove(a.method);
	save();
	refresh();
	setStatus(a.label + " cleared.");
}

void SettingsScreen::onDebounce(int ms)
{
	cfg.setDebounce(ms);
	save();
	refresh();
	setStatus(ms == 0 ? QString("Repeat presses are no longer ignored.") :
	          QString("Presses within %1 ms of the last one are ignored.")
	          .arg(ms));
}

void SettingsScreen::onNextRemote()
{
	QList<RemoteInfo> remotes = btpt->remotes();
	if (remotes.isEmpty()) {
		return;
	}
	int next = 0;
	for (int i = 0; i < remotes.size(); i++) {
		if (remotes[i].uniq == uniq && remotes[i].name == name) {
			next = (i + 1) % remotes.size();
		}
	}
	stopCapture();
	loadRemote(remotes[next].uniq, remotes[next].name);
	setStatus("Showing " + remoteTitle() + ".");
}

void SettingsScreen::onIgnore()
{
	Config ignored;
	ignored.parse(QString(
		"# %1 isn't a page turner, so its buttons are left alone.\n"
		"# Delete this file to set it up again.\n")
		.arg(remoteTitle()).toUtf8());
	if (ignored.save(path)) {
		btpt->requestReload();
		deleteLater();
	}
	else {
		setStatus("Couldn't save " + path + ".");
	}
}

void SettingsScreen::onRemotesChanged()
{
	QList<RemoteInfo> remotes = btpt->remotes();
	if (path.isEmpty() && !remotes.isEmpty()) {
		loadRemote(remotes.first().uniq, remotes.first().name);
		setStatus(remoteTitle() + " connected. Tap Add next to an "
		          "action, then press the button you want for it.");
		return;
	}
	refresh();
}

bool SettingsScreen::eventFilter(QObject *obj, QEvent *event)
{
	/* Follow the main window when the screen rotates */
	if (obj == parent() && event->type() == QEvent::Resize) {
		setGeometry(parentWidget()->rect());
		applyScale();
	}
	return QWidget::eventFilter(obj, event);
}

SettingsLauncher::SettingsLauncher(BluetoothPageTurner *btpt)
	: btpt(btpt)
{
}

void SettingsLauncher::open(const QString &uniq)
{
	show(uniq, false);
}

void SettingsLauncher::offerSetup(const QString &uniq, const QString &name)
{
	show(uniq.isEmpty() ? name : uniq, true);
}

void SettingsLauncher::show(const QString &uniq, bool newRemote)
{
	if (screen) {
		if (!uniq.isEmpty()) {
			screen->showRemote(uniq);
		}
		screen->raise();
		return;
	}

	QWidget *window = nickelMainWindow();
	if (!window) {
		nh_log("no main window for settings");
		return;
	}
	/* List what else the reading view can do, for mapping by hand */
	QWidget *view = nickelCurrentView();
	if (view && view->objectName() == "ReadingView") {
		dumpMethods(view);
	}

	screen = new SettingsScreen(btpt, window, uniq, newRemote);
	screen->show();
	screen->raise();
}
