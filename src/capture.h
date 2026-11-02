#ifndef BTPT_CAPTURE_H
#define BTPT_CAPTURE_H

#include <QList>

struct RawEvent
{
	int type;
	int code;
	int value;
	/* Axis range, for EV_ABS events */
	int absMin;
	int absMax;

	bool same(const RawEvent &o) const
	{
		return type == o.type && code == o.code && value == o.value;
	}
};

/* Choose the event in a burst (everything a remote sent for one press) that
 * best identifies that button.
 *
 * A press can send several events: a gamepad d-pad sends the axis moving
 * out and back to centre, and a ring that pretends to be a touchscreen sends
 * BTN_TOUCH and a series of positions for every swipe.  Key presses are
 * preferred, then relative motion, then the axis value furthest from the
 * middle of its range; touch framing events come last.
 *
 * Events in exclude (for example, ones that other buttons also send) are
 * avoided when anything else is left.  Returns -1 if the burst has nothing
 * usable. */
int pickEvent(const QList<RawEvent> &burst, const QList<RawEvent> &exclude);

/* Events that never identify a button (sync reports, scan codes, LEDs) */
bool isNoise(int type);

#endif /* BTPT_CAPTURE_H */
