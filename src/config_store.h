#pragma once
#include <stdbool.h>
#include "bluebridge_state.h"
bool bb_config_load(bb_config_t* cfg);
bool bb_config_save(const bb_config_t* cfg);
