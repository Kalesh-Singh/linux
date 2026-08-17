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

#define TEST_FILE "/tmp/ppps_test_file.bin"

int main(void)
{
    long ps = sysconf(_SC_PAGE_SIZE);
    printf("[TEST_FILE] Starting file-backed slice test with page_size=%ld\n", ps);

    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);

    size_t file_len = 64 * 1024;
    char *init_buf = malloc(file_len);
    assert(init_buf);
    for (size_t i = 0; i < file_len; i++) {
        init_buf[i] = (char)((i / 4096) + 'A');
    }
    ssize_t written = write(fd, init_buf, file_len);
    assert(written == (ssize_t)file_len);
    free(init_buf);

    if (ps == 16384) {
        char *unaligned_map = mmap(NULL, 16384, PROT_READ, MAP_SHARED, fd, 4096);
        assert(unaligned_map == MAP_FAILED);
        assert(errno == EINVAL);

        char *aligned_map = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(aligned_map != MAP_FAILED);
        assert((unsigned char)aligned_map[0] == 'A');
        assert((unsigned char)aligned_map[4096] == 'B');
        aligned_map[4096] = 'Y';
        msync(aligned_map, 16384, MS_SYNC);
        munmap(aligned_map, 16384);

        char c;
        pread(fd, &c, 1, 4096);
        assert(c == 'Y');
    } else {
        char *map1 = mmap(NULL, 4096 * 3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
        assert(map1 != MAP_FAILED);

        assert((unsigned char)map1[0] == 'B' || (unsigned char)map1[0] == 'Y');
        assert((unsigned char)map1[4096] == 'C');
        assert((unsigned char)map1[8192] == 'D');

        map1[0] = 'X';
        msync(map1, 4096, MS_SYNC);
        munmap(map1, 4096 * 3);

        char *map2 = mmap(NULL, 4096 * 2, PROT_READ, MAP_SHARED, fd, 8192);
        assert(map2 != MAP_FAILED);
        assert((unsigned char)map2[0] == 'C');
        assert((unsigned char)map2[4096] == 'D');
        munmap(map2, 4096 * 2);

        char verify_c;
        ssize_t pr = pread(fd, &verify_c, 1, 4096);
        assert(pr == 1);
        assert(verify_c == 'X');
    }

    close(fd);
    unlink(TEST_FILE);

    printf("[TEST_FILE] PASS\n");
    return 0;
}
