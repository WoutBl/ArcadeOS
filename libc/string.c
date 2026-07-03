#include "string.h"

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

void strcpy(char* dest, const char* src) {
    size_t i = 0;
    while (src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

void num_to_str(char* str, int num, int min_digits) {
    char temp[20];
    int i = 0;
    int is_neg = 0;
    
    if (num < 0) {
        is_neg = 1;
        num = -num;
    }

    if (num == 0) {
        temp[i++] = '0';
    } else {
        while (num > 0) {
            temp[i++] = '0' + (char)(num % 10);
            num /= 10;
        }
    }
    
    while (i < min_digits) temp[i++] = '0';
    if (is_neg) temp[i++] = '-';

    int j = 0;
    while (i > 0) str[j++] = temp[--i];
    str[j] = '\0';
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}
