// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <assert.h>

int main(void)
{
    long ps = sysconf(_SC_PAGE_SIZE);
    printf("[TEST_FORK] Starting fork test with page_size=%ld\n", ps);

    char *buf = mmap(NULL, ps * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(buf != MAP_FAILED);
    memset(buf, 'P', ps * 4);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        long child_ps = sysconf(_SC_PAGE_SIZE);
        assert(child_ps == ps);
        assert(buf[0] == 'P');
        assert(buf[ps] == 'P');

        buf[0] = 'C';
        assert(buf[0] == 'C');
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    assert(buf[0] == 'P');

    munmap(buf, ps * 4);
    printf("[TEST_FORK] PASS\n");
    return 0;
}
