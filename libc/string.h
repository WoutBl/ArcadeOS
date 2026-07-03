#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stddef.h>

size_t strlen(const char* str);
void strcpy(char* dest, const char* src);
void num_to_str(char* str, int num, int min_digits);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);

#endif /* LIBC_STRING_H */
