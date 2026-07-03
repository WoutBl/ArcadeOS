#ifndef TYPES_H
#define TYPES_H

#include <stddef.h>
#include <stdint.h>

/* OS Information */
#define OS_NAME "ArcadeOS"
#define OS_VERSION "1.0.0"
#define OS_DESCRIPTION "A bare-metal gaming console operating system"

/* Filesystem limits */
#define MAX_FILES 32
#define MAX_FILENAME 64
#define MAX_FILE_SIZE 8192
#define MAX_PATH 128
#define CMD_BUFFER_SIZE 256

/* ──────────────────── Inline I/O port access ──────────────────── */

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline void insl(uint16_t port, uint32_t* addr, uint32_t count) {
    asm volatile("cld; rep insl" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outsl(uint16_t port, const uint32_t* addr, uint32_t count) {
    asm volatile("cld; rep outsl" : "+S"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void io_wait(void) {
    /* Write to an unused port to burn ~1 µs */
    outb(0x80, 0);
}

static inline void cli(void) { asm volatile("cli"); }
static inline void sti(void) { asm volatile("sti"); }
static inline void hlt(void) { asm volatile("hlt"); }

/* ──────────────────── String / memory helpers ──────────────────── */

size_t strlen(const char* str);
void   strcpy(char* dest, const char* src);
char*  strncpy(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strrchr(const char* str, int c);
void memset(void* ptr, int value, size_t num);
void memcpy(void* dest, const void* src, size_t n);
int memcmp(const void* s1, const void* s2, size_t n);
void num_to_str(char* str, uint32_t num, int min_digits);

#endif /* TYPES_H */
