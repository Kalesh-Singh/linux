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
    printf("[TEST_MREMAP] Starting mremap test with page_size=%ld\n", ps);

    size_t init_pages = 8;
    size_t init_size = init_pages * ps;

    /* 1. Allocate initial anonymous mapping */
    char *buf = mmap(NULL, init_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(buf != MAP_FAILED);

    /* 2. Write unique per-page pattern */
    for (size_t i = 0; i < init_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            buf[i * ps + j] = (char)(0x30 + i);
        }
    }

    /* 3. Expand mapping with MREMAP_MAYMOVE */
    size_t expanded_pages = 16;
    size_t expanded_size = expanded_pages * ps;
    char *new_buf = mremap(buf, init_size, expanded_size, MREMAP_MAYMOVE);
    assert(new_buf != MAP_FAILED);

    /* 4. Verify existing pages preserved */
    for (size_t i = 0; i < init_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            assert(new_buf[i * ps + j] == (char)(0x30 + i));
        }
    }

    /* 5. Write to newly expanded pages */
    for (size_t i = init_pages; i < expanded_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            new_buf[i * ps + j] = (char)(0x41 + (i - init_pages));
        }
    }

    /* 6. Verify entire expanded range */
    for (size_t i = 0; i < init_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            assert(new_buf[i * ps + j] == (char)(0x30 + i));
        }
    }
    for (size_t i = init_pages; i < expanded_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            assert(new_buf[i * ps + j] == (char)(0x41 + (i - init_pages)));
        }
    }

    /* 7. Shrink mapping back down to 4 pages */
    size_t shrink_pages = 4;
    size_t shrink_size = shrink_pages * ps;
    char *shrunk_buf = mremap(new_buf, expanded_size, shrink_size, 0);
    assert(shrunk_buf == new_buf);

    for (size_t i = 0; i < shrink_pages; i++) {
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            assert(shrunk_buf[i * ps + j] == (char)(0x30 + i));
        }
    }

    /* 8. Move a 2-page slice to a fixed location */
    void *target_hint = (void *)0x60000000UL;
    void *fixed_dest = mmap(target_hint, 2 * ps, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (fixed_dest != MAP_FAILED) {
        char *relocated = mremap(shrunk_buf + 1 * ps, 2 * ps, 2 * ps,
                                 MREMAP_MAYMOVE | MREMAP_FIXED, fixed_dest);
        assert(relocated == fixed_dest);
        for (size_t j = 0; j < (size_t)ps; j += 64) {
            assert(relocated[0 * ps + j] == (char)(0x30 + 1));
            assert(relocated[1 * ps + j] == (char)(0x30 + 2));
        }
        munmap(fixed_dest, 2 * ps);
    }

    munmap(shrunk_buf, 1 * ps);
    munmap(shrunk_buf + 3 * ps, 1 * ps);

    printf("[TEST_MREMAP] PASS\n");
    return 0;
}
