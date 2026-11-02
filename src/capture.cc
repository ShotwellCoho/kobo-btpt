#include <stdlib.h>

#include <linux/input-event-codes.h>

#include "capture.h"

bool isNoise(int type)
{
	switch (type) {
	case EV_SYN:
	case EV_MSC:
	case EV_LED:
	case EV_SND:
	case EV_REP:
	case EV_FF:
	case EV_PWR:
	case EV_FF_STATUS:
		return true;
	default:
		return false;
	}
}

/* Touch, pen and finger-count buttons, which accompany every gesture */
static bool isDigitizerButton(int code)
{
	return code >= BTN_DIGI && code <= BTN_DIGI + 0xf;
}

static int score(const RawEvent &e, const QList<RawEvent> &burst)
{
	switch (e.type) {
	case EV_KEY:
		if (e.value == 2) {
			return 0; /* autorepeat */
		}
		if (isDigitizerButton(e.code)) {
			return e.value == 1 ? 120 : 110;
		}
		if (e.value == 1) {
			return 1000;
		}
		/* A release is only useful if the press wasn't seen */
		foreach (const RawEvent &other, burst) {
			if (other.type == EV_KEY && other.code == e.code &&
			    other.value == 1) {
				return 0;
			}
		}
		return 900;
	case EV_REL:
		return e.value != 0 ? 800 : 0;
	case EV_SW:
		return 700;
	case EV_ABS: {
		if (e.code >= ABS_MT_SLOT) {
			return 130; /* multitouch details */
		}
		/* 500..600, further from the middle is better */
		long range = (long)e.absMax - e.absMin;
		if (range <= 0) {
			return 500;
		}
		long mid2 = (long)e.absMax + e.absMin;
		long dist2 = labs(2L * e.value - mid2);
		return 500 + (int)(100 * dist2 / range);
	}
	default:
		return 0;
	}
}

static bool contains(const QList<RawEvent> &list, const RawEvent &e)
{
	foreach (const RawEvent &other, list) {
		if (other.same(e)) {
			return true;
		}
	}
	return false;
}

static int best(const QList<RawEvent> &burst, const QList<RawEvent> &exclude)
{
	int bestIndex = -1;
	int bestScore = 0;
	for (int i = 0; i < burst.size(); i++) {
		const RawEvent &e = burst[i];
		if (isNoise(e.type) || contains(exclude, e)) {
			continue;
		}
		int s = score(e, burst);
		/* Strictly greater: earlier events win ties */
		if (s > bestScore) {
			bestScore = s;
			bestIndex = i;
		}
	}
	return bestIndex;
}

int pickEvent(const QList<RawEvent> &burst, const QList<RawEvent> &exclude)
{
	int i = best(burst, exclude);
	if (i < 0 && !exclude.isEmpty()) {
		i = best(burst, QList<RawEvent>());
	}
	return i;
}
