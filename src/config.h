#ifndef BTPT_CONFIG_H
#define BTPT_CONFIG_H

#include <QList>
#include <QString>
#include <QStringList>

/* Directory in which to look for device configuration */
#define BTPT_DIR "/mnt/onboard/.btpt/"

struct Mapping
{
	QString method;
	int type;
	int code;
	int value;

	bool matches(int t, int c, int v) const
	{
		return type == t && code == c && value == v;
	}
};

/* A device configuration file.
 *
 * Each line is one of:
 *
 *   METHOD TYPE CODE VALUE   invoke METHOD on ReadingView for this event
 *   debounce MILLISECONDS    ignore further matches for this long after one
 *   # comment                ignored, as are blank lines and trailing comments
 *
 * The raw lines are kept so that edits made by the settings screen leave
 * comments and anything else in the file alone. */
class Config
{
public:
	QList<Mapping> mappings;
	int debounceMs = 0;

	void parse(const QByteArray &text);
	QByteArray serialize() const;

	bool load(const QString &path);
	bool save(const QString &path) const;

	void setDebounce(int ms);
	void addMapping(const Mapping &mapping);
	void removeMethod(const QString &method);
	void removeEvent(int type, int code, int value);

	QList<Mapping> mappingsFor(const QString &method) const;

private:
	QStringList lines;
	void reparse();
};

/* Configuration file for a device: one named after the Bluetooth address
 * (case insensitive, no colons) wins over one named after the device name.
 * If neither exists, *exists is false and the address-named path is
 * returned so that new files are specific to this device. */
QString configPath(const QString &name, const QString &uniq, bool *exists);

/* "EV_KEY KEY_VOLUMEUP 1" */
QString formatEvent(int type, int code, int value);

#endif /* BTPT_CONFIG_H */
