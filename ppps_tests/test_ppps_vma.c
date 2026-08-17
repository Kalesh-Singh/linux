// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <assert.h>

#define TEST_FILE "/tmp/ppps_test_vma.bin"

int main(void)
{
    long ps = sysconf(_SC_PAGE_SIZE);
    printf("[TEST_VMA] Starting VMA merge/split test with page_size=%ld\n", ps);

    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);

    size_t file_len = 64 * 1024;
    char *buf = calloc(1, file_len);
    assert(buf);
    ssize_t w = write(fd, buf, file_len);
    assert(w == (ssize_t)file_len);
    free(buf);

    if (ps == 4096) {
        char *map = mmap(NULL, 12288, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
        assert(map != MAP_FAILED);

        int p = mprotect(map + 4096, 4096, PROT_READ);
        assert(p == 0);

        map[0] = 'A';
        assert(map[0] == 'A');

        map[8192] = 'C';
        assert(map[8192] == 'C');

        munmap(map, 12288);
    } else {
        char *map = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(map != MAP_FAILED);

        int p = mprotect(map + 16384, 16384, PROT_READ);
        assert(p == 0);

        map[0] = 'A';
        assert(map[0] == 'A');

        munmap(map, 32768);
    }

    close(fd);
    unlink(TEST_FILE);

    printf("[TEST_VMA] PASS\n");
    return 0;
}
