#include "stdio.h"
#include "syscall.h"
#include "string.h"
#include <stdarg.h>

char* gets(char* str, int max_len) {
    if (!str || max_len <= 1) return str;

    int bytes = read(0, str, max_len - 1);
    
    if (bytes >= 0) {
        /* Remove trailing newline if sys_read included it */
        if (bytes > 0 && str[bytes - 1] == '\n') {
            str[bytes - 1] = '\0';
        } else {
            str[bytes] = '\0';
        }
    } else {
        str[0] = '\0';
    }
    
    return str;
}

int printf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    int chars_written = 0;
    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%' && format[i+1] != '\0') {
            i++;
            if (format[i] == 's') {
                char* s = va_arg(args, char*);
                write(1, s, 0); 
                chars_written += strlen(s);
            } else if (format[i] == 'd') {
                int d = va_arg(args, int);
                char num_buf[32];
                num_to_str(num_buf, d, 1);
                write(1, num_buf, 0);
                chars_written += strlen(num_buf);
            } else if (format[i] == 'c') {
                char c = (char)va_arg(args, int);
                char buf[2] = {c, '\0'};
                write(1, buf, 0);
                chars_written++;
            } else if (format[i] == '%') {
                write(1, "%", 0);
                chars_written++;
            } else {
                /* Unknown format specifier, just print it */
                char buf[3] = {'%', format[i], '\0'};
                write(1, buf, 0);
                chars_written += 2;
            }
        } else {
            char buf[2] = {format[i], '\0'};
            write(1, buf, 0);
            chars_written++;
        }
    }
    va_end(args);
    return chars_written;
}
