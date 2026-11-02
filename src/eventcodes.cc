#include <QMap>
#include <QString>
#include <QStringList>

#include <map>

#include <linux/input-event-codes.h>

#include "eventcodes.h"

#define VALUE(code) code
#define MAP(code) map[#code] = VALUE(code)

static QMap<QString, int> init()
{
	QMap<QString, int> map;
#include "eventcodes_init.h"
	return map;
}

static QMap<QString, int> codes = init();

/* Parse C-style integer or #define from <linux/input-event-codes.h> */
int parseEventCode(bool *ok, const QString &s)
{
	int value = s.toInt(ok, 0);
	if (*ok) {
		return value;
	}
	else if (codes.contains(s)) {
		*ok = true;
		return codes[s];
	}

	*ok = false;
	return -1;
}

/* Find the first name (alphabetically) with one of the prefixes and value,
 * skipping range markers like KEY_MAX and ABS_CNT. */
static QString lookup(const QStringList &prefixes, int value)
{
	for (QMap<QString, int>::const_iterator it = codes.constBegin();
	     it != codes.constEnd(); ++it) {
		if (it.value() != value) {
			continue;
		}
		const QString &name = it.key();
		if (name.endsWith("_MAX") || name.endsWith("_CNT") ||
		    name.endsWith("_MIN_INTERESTING")) {
			continue;
		}
		foreach (const QString &prefix, prefixes) {
			if (name.startsWith(prefix)) {
				return name;
			}
		}
	}
	return QString();
}

QString eventTypeName(int type)
{
	QString name = lookup(QStringList() << "EV_", type);
	return name.isEmpty() ? QString::number(type) : name;
}

QString eventCodeName(int type, int code)
{
	QStringList prefixes;
	switch (type) {
	case EV_KEY:
		/* Prefer KEY_ names for keyboard keys, BTN_ for buttons */
		prefixes << (code >= BTN_MISC && code < KEY_OK ? "BTN_" : "KEY_")
		         << "BTN_" << "KEY_";
		break;
	case EV_REL: prefixes << "REL_"; break;
	case EV_ABS: prefixes << "ABS_"; break;
	case EV_MSC: prefixes << "MSC_"; break;
	case EV_SW:  prefixes << "SW_";  break;
	case EV_LED: prefixes << "LED_"; break;
	case EV_SND: prefixes << "SND_"; break;
	case EV_SYN: prefixes << "SYN_"; break;
	default: break;
	}
	QString name = prefixes.isEmpty() ? QString() : lookup(prefixes, code);
	return name.isEmpty() ? QString::number(code) : name;
}
