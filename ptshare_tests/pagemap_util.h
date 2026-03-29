#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int is_page_present(void* addr) {
    uint64_t entry;
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        perror("open pagemap");
        return -1;
    }
    
    uintptr_t vaddr = (uintptr_t)addr;
    off_t offset = (vaddr / 4096) * sizeof(uint64_t);
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        perror("lseek pagemap");
        close(fd);
        return -1;
    }
    
    if (read(fd, &entry, sizeof(uint64_t)) != sizeof(uint64_t)) {
        perror("read pagemap");
        close(fd);
        return -1;
    }
    
    close(fd);
    return (int)((entry >> 63) & 1);
}
