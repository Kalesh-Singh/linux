// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/personality.h>
#include <errno.h>

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <program> [args...]\n", argv[0]);
        return 1;
    }

    int current_pers = personality(0xffffffff);
    if (current_pers == -1) {
        perror("personality(0xffffffff)");
        return 1;
    }

    int new_pers = current_pers | ADDR_4KB_COMPAT_PAGE_SIZE;
    if (personality(new_pers) == -1) {
        perror("personality(ADDR_4KB_COMPAT_PAGE_SIZE)");
        return 1;
    }

    execvp(argv[1], &argv[1]);
    perror("execvp");
    return 1;
}
