#ifndef EVENTCODES_H
#define EVENTCODES_H

#include <QMap>
#include <QString>

int parseEventCode(bool *ok, const QString &s);

/* Symbolic names for writing configuration files, falling back to numbers */
QString eventTypeName(int type);
QString eventCodeName(int type, int code);

#endif /* EVENTCODES_H */
