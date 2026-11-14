#pragma once
#include <stdint.h>
#include <string.h>
#define FLASH_SECTOR_SIZE 4096u
#define PICO_FLASH_SIZE_BYTES (4u*1024u*1024u)
extern unsigned test_flash_erases;
extern uint8_t test_flash[PICO_FLASH_SIZE_BYTES];
#define XIP_BASE ((uintptr_t)test_flash)
static inline void flash_range_erase(uint32_t offset, size_t size) { ++test_flash_erases; memset(test_flash+offset,255,size); }
static inline void flash_range_program(uint32_t offset, const uint8_t* data, size_t size) { memcpy(test_flash+offset,data,size); }
