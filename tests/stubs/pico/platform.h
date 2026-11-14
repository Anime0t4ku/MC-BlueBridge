#pragma once
extern unsigned test_core;
static inline unsigned get_core_num(void) { return test_core; }
#define __not_in_flash_func(name) name
