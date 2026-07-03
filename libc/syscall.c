#include "syscall.h"

void exit(int code) {
    asm volatile(
        "int $0x80"
        :
        : "a"(0), "b"(code)
        : "memory"
    );
    while (1) { asm volatile("pause" ::: "memory"); }
}

int write(int fd, const char* buf, int len) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(1), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

int read(int fd, char* buf, int len) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(3), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

void yield(void) {
    asm volatile(
        "int $0x80"
        :
        : "a"(2)
        : "memory"
    );
}

int spawn(const char* path, char* const argv[]) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "b"(path), "c"(argv)
        : "memory"
    );
    return ret;
}

int wait(int pid) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(5), "b"(pid)
        : "memory"
    );
    return ret;
}

int listdir(char* buf, int max_len) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(6), "b"(buf), "c"(max_len)
        : "memory"
    );
    return ret;
}

int readfile(const char* path, char* buf, int max_len) {
    int ret;
    asm volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(7), "b"(path), "c"(buf), "d"(max_len)
        : "memory"
    );
    return ret;
}

int touch(const char* path) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(8), "b"(path) : "memory");
    return ret;
}

int rm(const char* path) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(9), "b"(path) : "memory");
    return ret;
}

int mkdir(const char* path) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(10), "b"(path) : "memory");
    return ret;
}

int cd(const char* path) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(11), "b"(path) : "memory");
    return ret;
}

int pwd(char* buf, int max_len) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(12), "b"(buf), "c"(max_len) : "memory");
    return ret;
}

int writefile(const char* path, const char* content) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(13), "b"(path), "c"(content) : "memory");
    return ret;
}

int getdate(char* buf, int max_len) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(14), "b"(buf), "c"(max_len) : "memory");
    return ret;
}

int settheme(int theme_id) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(15), "b"(theme_id) : "memory");
    return ret;
}

int open(const char* path, int flags) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(16), "b"(path), "c"(flags) : "memory");
    return ret;
}

int close(int fd) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(17), "b"(fd) : "memory");
    return ret;
}

int pipe(int pipefd[2]) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(18), "b"(pipefd) : "memory");
    return ret;
}

int dup2(int oldfd, int newfd) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(19), "b"(oldfd), "c"(newfd) : "memory");
    return ret;
}

int signal(int signum, void* handler) {
    int ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(20), "b"(signum), "c"(handler) : "memory");
    return ret;
}
