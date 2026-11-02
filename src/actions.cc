/* Device actions.
 *
 * The Nickel calls here follow NickelMenu's power, nickel_setting and
 * nickel_orientation actions (MIT License, Copyright (c) 2020-2025 Patrick
 * Gaskin), including the Qt 6 Settings layout checks from the qt6-support
 * branch of nicoverbruggen/NickelMenu. */

#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <string.h>

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QGuiApplication>
#include <QMetaMethod>
#include <QMetaObject>
#include <QScreen>
#include <QShowEvent>
#include <QTextStream>
#include <QWidget>

#include <NickelHook.h>

#include "actions.h"
#include "btpt.h"
#include "config.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
/* On Qt 6 firmware, check that a Settings subclass's vtable looks the way
 * the code below assumes before swapping it in, so a firmware change makes
 * the action fail instead of crashing Nickel.  Both inspected layouts have
 * the Itanium header and five virtual functions. */
static bool vtableLooksRight(void *address, const char *name)
{
	Dl_info info;
	void *extra = NULL;
	if (!dladdr1(address, &info, &extra, RTLD_DL_SYMENT) || !extra) {
		return false;
	}
	const ElfW(Sym) *symbol = static_cast<const ElfW(Sym) *>(extra);
	if (symbol->st_size != 7 * sizeof(void *)) {
		return false;
	}

	const QByteArray type = name + 4;
	const QByteArray names[] = {
		"_ZTI" + type,
		"_ZN" + type + "D1Ev",
		"_ZN" + type + "D0Ev",
		"_ZN8Settings12clearSectionEv",
		type == "15ReadingSettings"
			? "_ZN15ReadingSettings11saveSettingERK7QStringRK8QVariantb"
			: "_ZN8Settings11saveSettingERK7QStringRK8QVariantb",
		"_ZNK" + type + "11sectionNameEv",
	};
	void *const *entries = static_cast<void *const *>(address);
	if (entries[0]) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		void *expected = dlsym(RTLD_DEFAULT, names[i].constData());
		if (!expected || entries[i + 1] != expected) {
			return false;
		}
	}
	return true;
}
#endif

static void *resolve(const char *name)
{
	void *address = dlsym(RTLD_DEFAULT, name);
	if (!address) {
		nh_log("missing symbol %s", name);
		return NULL;
	}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	if (!strncmp(name, "_ZTV", 4) && !vtableLooksRight(address, name)) {
		nh_log("unexpected layout for %s", name);
		return NULL;
	}
#endif
	return address;
}

#define RESOLVE(var, name) \
	reinterpret_cast<void *&>(var) = resolve(name)

/* A Nickel Settings object, reinterpreted as one of its subclasses by
 * pointing its vtable at the subclass's.  Calls may change the vtable
 * pointer, so as() is used before each one. */
class NickelSettings
{
public:
	NickelSettings()
	{
		void *(*getCurrentDevice)();
		void *(*construct)(void *, void *, bool);
		RESOLVE(getCurrentDevice, "_ZN6Device16getCurrentDeviceEv");
		RESOLVE(construct, "_ZN8SettingsC2ERK6Deviceb");
		RESOLVE(destroy, "_ZN8SettingsD2Ev");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
		void *(*constructLegacy)(void *, void *) = NULL;
		if (!construct) {
			RESOLVE(constructLegacy, "_ZN8SettingsC2ERK6Device");
		}
#endif
		void *vtable = resolve("_ZTV8Settings");
		if (!getCurrentDevice || !destroy || !vtable) {
			return;
		}
		void *device = getCurrentDevice();
		if (!device) {
			nh_log("no current Device");
			return;
		}

		if (construct) {
			construct(storage, device, false);
		}
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
		else if (constructLegacy) {
			constructLegacy(storage, device);
		}
#endif
		else {
			return;
		}
		constructed = true;

		if (*reinterpret_cast<void **>(storage) != target(vtable)) {
			nh_log("unexpected Settings vtable layout");
			return;
		}
		usable = true;
	}

	~NickelSettings()
	{
		if (constructed) {
			destroy(storage);
		}
	}

	bool ok() const { return usable; }

	void *as(void *vtable)
	{
		*reinterpret_cast<void **>(storage) = target(vtable);
		return storage;
	}

private:
	/* Much larger than a Settings, to be safe */
	void *storage[64];
	void (*destroy)(void *) = NULL;
	bool constructed = false;
	bool usable = false;

	static void *target(void *vtable)
	{
		/* Objects point 8 bytes into their vtable, past the header */
		return reinterpret_cast<void *>(
			reinterpret_cast<uintptr_t>(vtable) + 8);
	}
};

static bool requestSleep()
{
	void *(*sharedInstance)();
	void (*sleepNow)(void *);
	RESOLVE(sharedInstance, "_ZN22N3PowerWorkflowManager14sharedInstanceEv");
	RESOLVE(sleepNow, "_ZN22N3PowerWorkflowManager12requestSleepEv");
	if (!sharedInstance || !sleepNow) {
		return false;
	}
	void *pwm = sharedInstance();
	if (!pwm) {
		nh_log("no N3PowerWorkflowManager");
		return false;
	}
	sleepNow(pwm);
	return true;
}

static bool toggleInvert(NickelSettings &settings)
{
	void *vtable = resolve("_ZTV15FeatureSettings");
	bool (*get)(void *);
	void (*set)(void *, bool);
	RESOLVE(get, "_ZN15FeatureSettings12invertScreenEv");
	RESOLVE(set, "_ZN15FeatureSettings15setInvertScreenEb");
	if (!vtable || !get || !set) {
		return false;
	}

	bool v = get(settings.as(vtable));
	set(settings.as(vtable), !v);
	bool now = get(settings.as(vtable));
	settings.as(vtable);

	QWidget *w = QApplication::topLevelAt(25, 25);
	if (w) {
		w->update();
	}
	return now == !v;
}

static bool toggleNightMode()
{
	NickelSettings settings;
	if (!settings.ok()) {
		return false;
	}

	void *vtable = resolve("_ZTV15ReadingSettings");
	bool (*get)(void *);
	void (*set)(void *, bool);
	RESOLVE(get, "_ZN15ReadingSettings11getDarkModeEv");
	RESOLVE(set, "_ZN15ReadingSettings11setDarkModeEb");
	if (!vtable || !get || !set) {
		/* Firmware before dark mode (4.28) can still invert */
		nh_log("no dark mode, trying invert");
		return toggleInvert(settings);
	}

	bool v = get(settings.as(vtable));
	set(settings.as(vtable), !v);
	bool now = get(settings.as(vtable));
	settings.as(vtable);

	/* Redraw the book in the new mode */
	QWidget *view = nickelCurrentView();
	if (view && view->objectName() == "ReadingView") {
		QShowEvent ev;
		QApplication::sendEvent(view, &ev);
	}
	return now == !v;
}

static bool rotate()
{
	QScreen *screen = QGuiApplication::primaryScreen();
	if (!screen) {
		return false;
	}

	Qt::ScreenOrientation o;
	switch (screen->orientation()) {
	case Qt::PortraitOrientation:
		o = Qt::LandscapeOrientation;
		break;
	case Qt::LandscapeOrientation:
		o = Qt::PortraitOrientation;
		break;
	case Qt::InvertedPortraitOrientation:
		o = Qt::InvertedLandscapeOrientation;
		break;
	case Qt::InvertedLandscapeOrientation:
		o = Qt::InvertedPortraitOrientation;
		break;
	default:
		nh_log("unknown screen orientation %d", (int)screen->orientation());
		return false;
	}

	void (*orientationChange)(QScreen *, Qt::ScreenOrientation);
	RESOLVE(orientationChange,
		"_ZN22QWindowSystemInterface29handleScreenOrientationChangeEP7QScreenN2Qt17ScreenOrientationE");
	void *vtable = resolve("_ZTV19ApplicationSettings");
	void (*setLocked)(void *, int);
	RESOLVE(setLocked,
		"_ZN19ApplicationSettings20setLockedOrientationE6QFlagsIN2Qt17ScreenOrientationEE");
	if (!orientationChange || !vtable || !setLocked) {
		return false;
	}

	NickelSettings settings;
	if (!settings.ok()) {
		return false;
	}

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	/* Stop the orientation sensor overriding this straight away */
	screen->setOrientationUpdateMask(o);
#endif

	/* Allow the new orientation, or it may be ignored */
	setLocked(settings.as(vtable), o);
	settings.as(vtable);

	orientationChange(screen, o);
	nh_log("rotated; if nothing happened, the current view may not allow "
	       "landscape (see [DeveloperSettings] ForceAllowLandscape)");
	return true;
}

bool isDeviceAction(const QString &method)
{
	return method.startsWith("nickel:");
}

bool runDeviceAction(const QString &method)
{
	bool ok;
	if (method == "nickel:sleep") {
		ok = requestSleep();
	}
	else if (method == "nickel:nightMode") {
		ok = toggleNightMode();
	}
	else if (method == "nickel:rotate") {
		ok = rotate();
	}
	else {
		nh_log("unknown action %s", qPrintable(method));
		return false;
	}
	if (!ok) {
		nh_log("%s isn't supported on this firmware", qPrintable(method));
	}
	return ok;
}

void dumpMethods(QWidget *view)
{
	if (!view) {
		return;
	}
	QFile file(BTPT_DIR "methods.txt");
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return;
	}
	QTextStream out(&file);
	out << "# Methods of Nickel's " << view->objectName() << " ("
	    << view->metaObject()->className() << ").\n"
	    << "# Ones marked * take no arguments, so they can be used as the\n"
	    << "# METHOD in a configuration file.  Untested ones may do anything.\n";
	const QMetaObject *mo = view->metaObject();
	for (int i = QWidget::staticMetaObject.methodCount();
	     i < mo->methodCount(); i++) {
		QMetaMethod m = mo->method(i);
		if (m.methodType() == QMetaMethod::Signal) {
			continue;
		}
		out << (m.parameterCount() == 0 ? "* " : "  ")
		    << m.methodSignature() << '\n';
	}
	nh_log("wrote %s", BTPT_DIR "methods.txt");
}
