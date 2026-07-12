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

