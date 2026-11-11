// LittleFS stand-in: the same desktop folder store as FFat.h (files live in
// sim/state/ffat/), under the name the firmware uses.
#pragma once
#include "FFat.h"

inline FFatFS &LittleFS = FFat;
