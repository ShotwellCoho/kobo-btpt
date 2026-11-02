#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>

#include <NickelHook.h>

#include "config.h"
#include "eventcodes.h"

static QStringList splitWords(const QString &line)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	return line.simplified().split(' ', Qt::SkipEmptyParts);
#else
	return line.simplified().split(' ', QString::SkipEmptyParts);
#endif
}

void Config::parse(const QByteArray &text)
{
	lines = QString::fromUtf8(text).split('\n');
	/* A trailing newline is not an extra line */
	if (!lines.isEmpty() && lines.last().isEmpty()) {
		lines.removeLast();
	}
	reparse();
}

void Config::reparse()
{
	mappings.clear();
	debounceMs = 0;

	foreach (QString line, lines) {
		int hash = line.indexOf('#');
		if (hash >= 0) {
			line.truncate(hash);
		}
		QStringList parts = splitWords(line);
		if (parts.isEmpty()) {
			continue;
		}

		bool ok;
		if (parts.size() == 2 && parts[0] == "debounce") {
			int ms = parts[1].toInt(&ok, 0);
			if (ok && ms >= 0) {
				debounceMs = ms;
			}
			else {
				nh_log("invalid debounce: %s", qPrintable(parts[1]));
			}
			continue;
		}

		if (parts.size() != 4) {
			nh_log("invalid config line: %s", qPrintable(line));
			continue;
		}

		Mapping m;
		m.method = parts[0];
		m.type = parseEventCode(&ok, parts[1]);
		if (!ok) {
			nh_log("invalid type: %s", qPrintable(parts[1]));
			continue;
		}
		m.code = parseEventCode(&ok, parts[2]);
		if (!ok) {
			nh_log("invalid code: %s", qPrintable(parts[2]));
			continue;
		}
		m.value = parseEventCode(&ok, parts[3]);
		if (!ok) {
			nh_log("invalid value: %s", qPrintable(parts[3]));
			continue;
		}
		mappings.append(m);
	}
}

QByteArray Config::serialize() const
{
	QString text;
	foreach (const QString &line, lines) {
		text += line + '\n';
	}
	return text.toUtf8();
}

bool Config::load(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		parse(QByteArray());
		return false;
	}
	parse(file.readAll());
	return true;
}

bool Config::save(const QString &path) const
{
	/* Write a temporary file and rename it, so the reader thread never
	 * sees a half-written file. */
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		nh_log("unable to write %s", qPrintable(path));
		return false;
	}
	file.write(serialize());
	if (!file.commit()) {
		nh_log("unable to write %s", qPrintable(path));
		return false;
	}
	return true;
}

/* The part of a line before any comment, split into words */
static QStringList lineWords(const QString &line)
{
	QString code = line;
	int hash = code.indexOf('#');
	if (hash >= 0) {
		code.truncate(hash);
	}
	return splitWords(code);
}

void Config::setDebounce(int ms)
{
	QString line = QString("debounce %1").arg(ms);
	for (int i = 0; i < lines.size(); i++) {
		QStringList parts = lineWords(lines[i]);
		if (parts.size() == 2 && parts[0] == "debounce") {
			lines[i] = line;
			reparse();
			return;
		}
	}

	/* Put it after any leading comments */
	int i = 0;
	while (i < lines.size() && lines[i].trimmed().startsWith('#')) {
		i++;
	}
	lines.insert(i, line);
	reparse();
}

void Config::addMapping(const Mapping &m)
{
	foreach (const Mapping &existing, mappings) {
		if (existing.method == m.method &&
		    existing.matches(m.type, m.code, m.value)) {
			return;
		}
	}
	lines.append(m.method + ' ' + formatEvent(m.type, m.code, m.value));
	reparse();
}

void Config::removeMethod(const QString &method)
{
	for (int i = lines.size() - 1; i >= 0; i--) {
		QStringList parts = lineWords(lines[i]);
		if (parts.size() == 4 && parts[0] == method) {
			lines.removeAt(i);
		}
	}
	reparse();
}

void Config::removeEvent(int type, int code, int value)
{
	for (int i = lines.size() - 1; i >= 0; i--) {
		QStringList parts = lineWords(lines[i]);
		if (parts.size() != 4) {
			continue;
		}
		bool ok1, ok2, ok3;
		int t = parseEventCode(&ok1, parts[1]);
		int c = parseEventCode(&ok2, parts[2]);
		int v = parseEventCode(&ok3, parts[3]);
		if (ok1 && ok2 && ok3 && t == type && c == code && v == value) {
			lines.removeAt(i);
		}
	}
	reparse();
}

QList<Mapping> Config::mappingsFor(const QString &method) const
{
	QList<Mapping> result;
	foreach (const Mapping &m, mappings) {
		if (m.method == method) {
			result.append(m);
		}
	}
	return result;
}

QString configPath(const QString &name, const QString &uniq, bool *exists)
{
	QStringList list = QDir(BTPT_DIR).entryList(QDir::Files);

	if (!uniq.isEmpty()) {
		foreach (const QString &filename, list) {
			if (filename.toLower() == uniq.toLower()) {
				*exists = true;
				return BTPT_DIR + filename;
			}
		}
	}

	if (!name.isEmpty() && list.contains(name)) {
		*exists = true;
		return BTPT_DIR + name;
	}

	*exists = false;
	return BTPT_DIR + (uniq.isEmpty() ? name : uniq.toUpper());
}

QString formatEvent(int type, int code, int value)
{
	return eventTypeName(type) + ' ' + eventCodeName(type, code) + ' ' +
	       QString::number(value);
}
