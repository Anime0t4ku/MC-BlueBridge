#pragma once
typedef struct { int unused; } critical_section_t;
static inline void critical_section_init(critical_section_t* p) { (void)p; }
static inline void critical_section_enter_blocking(critical_section_t* p) { (void)p; }
static inline void critical_section_exit(critical_section_t* p) { (void)p; }
