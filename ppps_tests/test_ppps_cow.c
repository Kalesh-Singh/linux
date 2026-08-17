// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>

#define TEST_FILE "/tmp/ppps_test_cow.bin"

int main(void)
{
    long ps = sysconf(_SC_PAGE_SIZE);
    printf("[TEST_COW] Starting CoW test with page_size=%ld\n", ps);

    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);

    size_t file_len = 32 * 1024;
    char *init_buf = malloc(file_len);
    assert(init_buf);
    for (size_t i = 0; i < file_len; i++) {
        init_buf[i] = (char)((i / 4096) + 'A');
    }
    ssize_t written = write(fd, init_buf, file_len);
    assert(written == (ssize_t)file_len);
    free(init_buf);

    if (ps == 16384) {
        char *cow_map = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        assert(cow_map != MAP_FAILED);
        assert((unsigned char)cow_map[0] == 'A');
        assert((unsigned char)cow_map[4096] == 'B');

        cow_map[4096] = 'Z';
        assert((unsigned char)cow_map[4096] == 'Z');

        char c;
        pread(fd, &c, 1, 4096);
        assert(c == 'B');
        munmap(cow_map, 16384);
    } else {
        char *cow_map = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 4096);
        assert(cow_map != MAP_FAILED);

        assert((unsigned char)cow_map[0] == 'B');
        assert((unsigned char)cow_map[4096] == 'C');

        cow_map[0] = 'Z';
        assert((unsigned char)cow_map[0] == 'Z');
        assert((unsigned char)cow_map[4096] == 'C');

        char c;
        pread(fd, &c, 1, 4096);
        assert(c == 'B');
        munmap(cow_map, 8192);
    }

    close(fd);
    unlink(TEST_FILE);

    printf("[TEST_COW] PASS\n");
    return 0;
}
