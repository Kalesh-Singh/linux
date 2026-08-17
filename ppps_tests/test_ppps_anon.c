// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>

int main(void)
{
    long ps = sysconf(_SC_PAGE_SIZE);
    printf("[TEST_ANON] Starting anon test with page_size=%ld\n", ps);

    size_t nr_pages = 8;
    size_t total_size = nr_pages * ps;

    char *buf = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(buf != MAP_FAILED);

    for (size_t i = 0; i < nr_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 256) {
            assert(buf[i * ps + j] == 0);
        }
        memset(buf + i * ps, (int)(i + 0x41), ps);
    }

    for (size_t i = 0; i < nr_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            assert((unsigned char)buf[i * ps + j] == (unsigned char)(i + 0x41));
        }
    }

    int ret = munmap(buf + 2 * ps, 2 * ps);
    assert(ret == 0);

    for (size_t j = 0; j < (size_t)ps; j += 64) {
        assert((unsigned char)buf[0 * ps + j] == 'A');
        assert((unsigned char)buf[1 * ps + j] == 'B');
        assert((unsigned char)buf[4 * ps + j] == 'E');
        assert((unsigned char)buf[5 * ps + j] == 'F');
    }

    munmap(buf, 2 * ps);
    munmap(buf + 4 * ps, 4 * ps);

    printf("[TEST_ANON] PASS\n");
    return 0;
}
