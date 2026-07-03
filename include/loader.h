#ifndef LOADER_H
#define LOADER_H

#include "types.h"

int exec(const char* filename, char* const argv[]);
int launch_user_app(const char* filename, char* const argv[]);

#endif /* LOADER_H */
