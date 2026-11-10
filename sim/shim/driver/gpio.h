#pragma once
#include "../../SimHardware.h"
typedef int gpio_num_t;
inline int gpio_get_level(gpio_num_t pin) { return sim::readPin(pin); }
