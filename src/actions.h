#ifndef BTPT_ACTIONS_H
#define BTPT_ACTIONS_H

#include <QString>

class QWidget;

/* Things a button can do besides call a ReadingView method.  In
 * configuration files they're written as the method, for example:
 *
 *   nickel:sleep EV_KEY KEY_POWER 1
 *
 *   nickel:sleep      put the Kobo to sleep
 *   nickel:nightMode  toggle dark mode (or invert, on firmware without it)
 *   nickel:rotate     swap between portrait and landscape
 *
 * Unlike page turning, these work in any view, not only while reading. */

bool isDeviceAction(const QString &method);

/* Run from the GUI thread.  Returns false (and logs why) if this firmware
 * doesn't support the action. */
bool runDeviceAction(const QString &method);

/* Write the ReadingView's invokable methods to BTPT_DIR, to find more
 * things to map. */
void dumpMethods(QWidget *view);

#endif /* BTPT_ACTIONS_H */
