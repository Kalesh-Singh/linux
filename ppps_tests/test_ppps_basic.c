// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/personality.h>
#include <sys/mman.h>
#include <assert.h>

#ifndef ADDR_4KB_COMPAT_PAGE_SIZE
#define ADDR_4KB_COMPAT_PAGE_SIZE 0x10000000
#endif

int main(void)
{
    long ps = sysconf(_SC_PAGE_SIZE);
    int pers = personality(0xffffffff);

    printf("[TEST_BASIC] Running with page size = %ld bytes\n", ps);
    printf("[TEST_BASIC] Personality = 0x%x\n", pers);

    if (pers & ADDR_4KB_COMPAT_PAGE_SIZE) {
        printf("[TEST_BASIC] Detected 4KB compat personality\n");
        assert(ps == 4096);
    } else {
        printf("[TEST_BASIC] Detected native personality\n");
        assert(ps == 16384 || ps == 4096);
    }

    void *ptr = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(ptr != MAP_FAILED);
    ((char *)ptr)[0] = 'A';
    ((char *)ptr)[ps - 1] = 'Z';
    assert(((char *)ptr)[0] == 'A');
    assert(((char *)ptr)[ps - 1] == 'Z');
    munmap(ptr, ps);

    printf("[TEST_BASIC] PASS\n");
    return 0;
}
