#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

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

struct rss_stats {
    long vm_rss;
    long rss_anon;
    long rss_file;
    long rss_shmem;
};

static int get_rss_stats(struct rss_stats* stats) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    
    char line[256];
    int found = 0;
    stats->vm_rss = stats->rss_anon = stats->rss_file = stats->rss_shmem = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &stats->vm_rss);
            found++;
        } else if (strncmp(line, "RssAnon:", 8) == 0) {
            sscanf(line + 8, "%ld", &stats->rss_anon);
            found++;
        } else if (strncmp(line, "RssFile:", 8) == 0) {
            sscanf(line + 8, "%ld", &stats->rss_file);
            found++;
        } else if (strncmp(line, "RssShmem:", 9) == 0) {
            sscanf(line + 9, "%ld", &stats->rss_shmem);
            found++;
        }
    }
    fclose(f);
    return (found == 4) ? 0 : -1;
}
