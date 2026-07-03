#ifndef LIBC_SYSCALL_H
#define LIBC_SYSCALL_H

#include <stddef.h>

#define SYS_OPEN  16
#define SYS_CLOSE 17
#define SYS_PIPE  18
#define SYS_DUP2  19

void exit(int code);
int write(int fd, const char* buf, int len);
int read(int fd, char* buf, int len);
void yield(void);

int spawn(const char* path, char* const argv[]);
int wait(int pid);
int listdir(char* buf, int max_len);
int readfile(const char* path, char* buf, int max_len);
int touch(const char* path);
int rm(const char* path);
int mkdir(const char* path);
int cd(const char* path);
int pwd(char* buf, int max_len);
int writefile(const char* path, const char* content);
int getdate(char* buf, int max_len);
int settheme(int theme_id);
int open(const char* path, int flags);
int close(int fd);
int pipe(int pipefd[2]);
int dup2(int oldfd, int newfd);
int signal(int signum, void* handler);

#endif /* LIBC_SYSCALL_H */
