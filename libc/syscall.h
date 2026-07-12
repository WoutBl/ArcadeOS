#ifndef LIBC_SYSCALL_H
#define LIBC_SYSCALL_H

#include <stddef.h>

#define SYS_OPEN  16
#define SYS_CLOSE 17

void exit(int code);
int write(int fd, const char* buf, int len);
int read(int fd, char* buf, int len);
void yield(void);

int spawn(const char* path, char* const argv[]);
int wait(int pid);
int open(const char* path, int flags);
int close(int fd);

#endif /* LIBC_SYSCALL_H */
