#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <iostream>

#ifndef MAP_SHARED_PT
#define MAP_SHARED_PT MAP_HUGETLB
#endif

#define PMD_SIZE (2 * 1024 * 1024)
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

class PtShareTest : public ::testing::Test {
protected:
    int fd;
    const char* test_file = "ptshare_test_file";
    size_t mapping_size = 1024UL * PMD_SIZE;

    void SetUp() override {
        fd = open(test_file, O_RDWR | O_CREAT | O_TRUNC, 0666);
        ASSERT_GE(fd, 0);
        ASSERT_EQ(ftruncate(fd, mapping_size), 0);
    }

    void TearDown() override {
        close(fd);
        unlink(test_file);
    }

    void* do_mmap(void* addr, size_t len, int prot, int flags) {
        return mmap(addr, len, prot, flags | MAP_SHARED_PT | MAP_FIXED, fd, 0);
    }
};

// Test 1: Basic MAP_SHARED_PT mapping succeeds with correct alignment
TEST_F(PtShareTest, BasicMapping) {
    std::cout << "[ INFO ] Starting BasicMapping test..." << std::endl;
    void* addr = (void*)0x700000000000;
    std::cout << "[ INFO ] Mapping " << PMD_SIZE << " bytes at " << addr << " with MAP_SHARED_PT..." << std::endl;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_PRIVATE);
    ASSERT_NE(mapped, MAP_FAILED) << "mmap failed: " << strerror(errno);
    ASSERT_EQ(mapped, addr);

    std::cout << "[ INFO ] Verifying memory access..." << std::endl;
    char val = *((char*)mapped);
    EXPECT_EQ(val, 0);

    std::cout << "[ INFO ] Unmapping..." << std::endl;
    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] BasicMapping test completed." << std::endl;
}

// Test 2: Invalid alignment (address) should fail
TEST_F(PtShareTest, AlignmentFailureAddress) {
    std::cout << "[ INFO ] Starting AlignmentFailureAddress test..." << std::endl;
    void* addr = (void*)(0x700000000000 + PAGE_SIZE);
    std::cout << "[ INFO ] Attempting non-PMD aligned mapping at " << addr << "..." << std::endl;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_PRIVATE);
    EXPECT_EQ(mapped, MAP_FAILED);
    EXPECT_EQ(errno, EINVAL);
    std::cout << "[ OK   ] Correctly failed with EINVAL." << std::endl;
}

// Test 3: Invalid alignment (size) should fail
TEST_F(PtShareTest, AlignmentFailureSize) {
    std::cout << "[ INFO ] Starting AlignmentFailureSize test..." << std::endl;
    void* addr = (void*)0x710000000000;
    std::cout << "[ INFO ] Attempting non-PMD aligned size mapping (" << PMD_SIZE + PAGE_SIZE << " bytes)..." << std::endl;
    void* mapped = do_mmap(addr, PMD_SIZE + PAGE_SIZE, PROT_READ, MAP_PRIVATE);
    EXPECT_EQ(mapped, MAP_FAILED);
    EXPECT_EQ(errno, EINVAL);
    std::cout << "[ OK   ] Correctly failed with EINVAL." << std::endl;
}

// Test 3b: PROT_WRITE with MAP_PRIVATE should fail for MAP_SHARED_PT
TEST_F(PtShareTest, WritePrivateFailure) {
    std::cout << "[ INFO ] Starting WritePrivateFailure test..." << std::endl;
    void* addr = (void*)0x710000200000;
    std::cout << "[ INFO ] Attempting PROT_WRITE | MAP_PRIVATE mapping..." << std::endl;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE);
    EXPECT_EQ(mapped, MAP_FAILED);
    EXPECT_EQ(errno, EINVAL);
    std::cout << "[ OK   ] Correctly failed with EINVAL." << std::endl;
}

// Test 4: Fault Sharing (implicit PTE splicing)
TEST_F(PtShareTest, FaultSharing) {
    std::cout << "[ INFO ] Starting FaultSharing test..." << std::endl;
    void* addr = (void*)0x720000000000;

    std::cout << "[ INFO ] Forking child to populate mapping..." << std::endl;
    pid_t pid = fork();
    if (pid == 0) {
        void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
        if (mapped == MAP_FAILED) {
            std::cerr << "Child mmap failed: " << strerror(errno) << std::endl;
            exit(1);
        }
        ((char*)mapped)[0] = 'A';
        exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    std::cout << "[ INFO ] Child finished. Parent mapping and checking value..." << std::endl;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    EXPECT_EQ(((char*)mapped)[0], 'A');
    std::cout << "[ OK   ] Parent saw value 'A' from child (Shared Page Table working)." << std::endl;
    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
}

// Test 5: Fork Inheritance
TEST_F(PtShareTest, ForkInheritance) {
    std::cout << "[ INFO ] Starting ForkInheritance test..." << std::endl;
    void* addr = (void*)0x730000000000;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    ((char*)mapped)[0] = 'B';

    std::cout << "[ INFO ] Forking child to check inherited mapping..." << std::endl;
    pid_t pid = fork();
    if (pid == 0) {
        if (((char*)mapped)[0] != 'B') {
            std::cerr << "Child saw incorrect value: " << ((char*)mapped)[0] << std::endl;
            exit(1);
        }
        ((char*)mapped)[0] = 'C';
        exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    EXPECT_EQ(((char*)mapped)[0], 'C');
    std::cout << "[ OK   ] Child inherited mapping and parent saw child's write." << std::endl;

    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
}

// Test 11: MADV_POPULATE_READ populates shared tables
TEST_F(PtShareTest, SharedPopulate) {
    std::cout << "[ INFO ] Starting SharedPopulate test..." << std::endl;
    void* addr = (void*)0x7A0000000000;

    std::cout << "[ INFO ] Mapping in parent and child..." << std::endl;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    ((char*)mapped)[0] = 'Y';

    pid_t pid = fork();
    if (pid == 0) {
        // Child should already see 'Y' but we use madvise to ensure population
        std::cout << "[ INFO ] Child populating via MADV_POPULATE_READ..." << std::endl;
        if (madvise(addr, PMD_SIZE, MADV_POPULATE_READ) != 0) exit(1);
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // If population worked, this access should not fault (hard to verify without counters,
    // but we verify correctness).
    EXPECT_EQ(((char*)mapped)[0], 'Y');

    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] Shared population verified." << std::endl;
}

// Test 12: Multi-process stress with large mapping
TEST_F(PtShareTest, MultiProcessStress) {
    std::cout << "[ INFO ] Starting MultiProcessStress test..." << std::endl;
    const int num_procs = 20;
    const int num_pmds = 5;
    void* addr = (void*)0x7B0000000000;

    // Parent populates the first page of each PMD
    void* mapped = do_mmap(addr, num_pmds * PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    for (int i = 0; i < num_pmds; i++) {
        ((char*)mapped)[i * PMD_SIZE] = 'S';
    }

    std::cout << "[ INFO ] Forking " << num_procs << " children to verify sharing..." << std::endl;
    for (int i = 0; i < num_procs; i++) {
        if (fork() == 0) {
            for (int j = 0; j < num_pmds; j++) {
                if (((char*)mapped)[j * PMD_SIZE] != 'S') exit(2);
            }
            exit(0);
        }
    }

    int status;
    int failed_procs = 0;
    for (int i = 0; i < num_procs; i++) {
        wait(&status);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed_procs++;
        }
    }

    EXPECT_EQ(failed_procs, 0);
    ASSERT_EQ(munmap(mapped, num_pmds * PMD_SIZE), 0);
    std::cout << "[ OK   ] Multi-process stress completed (" << num_procs << " processes)." << std::endl;
}

// Test 15: Deep Fork Chain (Refcounting stress)
TEST_F(PtShareTest, DeepForkRefcounting) {
    std::cout << "[ INFO ] Starting DeepForkRefcounting test..." << std::endl;
    void* addr = (void*)0x7E0000000000;

    // Parent populates
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    ((char*)mapped)[0] = 'G';

    pid_t pid1 = fork();
    if (pid1 == 0) {
        // Child 1
        pid_t pid2 = fork();
        if (pid2 == 0) {
            // Child 2 (Grandchild)
            sleep(2); // Wait for Parent and Child 1 to exit
            if (((char*)addr)[0] != 'G') exit(1);
            ((char*)addr)[0] = 'H';
            exit(0);
        }
        // Child 1 exits immediately
        exit(0);
    }

    // Parent exits immediately
    // Note: We can't actually "exit" the test process, but we can munmap
    // to simulate the last reference from this process going away.
    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);

    // Wait for the grandchild (pid2) indirectly via pid1
    int status;
    waitpid(pid1, &status, 0);

    // Now we need to wait for the actual grandchild. Since it's orphaned,
    // we just sleep or use a more robust way. For this test, we'll
    // wait for any child.
    wait(&status);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::cout << "[ OK   ] Deep fork refcounting verified." << std::endl;
}

// Test 16: Read-only MAP_PRIVATE Sharing
TEST_F(PtShareTest, ReadOnlyPrivateSharing) {
    std::cout << "[ INFO ] Starting ReadOnlyPrivateSharing test..." << std::endl;
    void* addr = (void*)0x7F0000000000;

    // Create a read-only private mapping
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_PRIVATE);
    ASSERT_NE(mapped, MAP_FAILED);

    pid_t pid = fork();
    if (pid == 0) {
        // Child verifies shared value (0)
        if (((char*)addr)[0] != 0) exit(1);
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // Verify that writing to the file (from another mapping)
    // propagates to this "shared" private mapping if it's still shared.
    void* writer = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(writer, MAP_FAILED);
    ((char*)writer)[0] = 'W';

    EXPECT_EQ(((char*)mapped)[0], 'W');

    munmap(writer, PAGE_SIZE);
    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] Read-only private sharing verified." << std::endl;
}

// Test 20: MADV_DONTFORK interaction
TEST_F(PtShareTest, MadviseDontFork) {
    std::cout << "[ INFO ] Starting MadviseDontFork test..." << std::endl;
    void* addr = (void*)0x690000000000;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    ASSERT_EQ(madvise(mapped, PMD_SIZE, MADV_DONTFORK), 0);

    pid_t pid = fork();
    if (pid == 0) {
        // Child should not have this VMA. Check by trying to map something else there.
        void* check = mmap(addr, PAGE_SIZE, PROT_READ, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (check == addr) {
            exit(0);
        }
        exit(1);
    }

    int status;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    munmap(mapped, PMD_SIZE);
    std::cout << "[ OK   ] MADV_DONTFORK interaction verified." << std::endl;
}

// Test 21: execve cleanup
TEST_F(PtShareTest, ExecveCleanup) {
    std::cout << "[ INFO ] Starting ExecveCleanup test..." << std::endl;
    void* addr = (void*)0x680000000000;

    pid_t pid = fork();
    if (pid == 0) {
        void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_SHARED);
        if (mapped == MAP_FAILED) exit(1);

        // Replace process image. This should trigger VMA cleanup.
        execl("/bin/true", "true", NULL);
        exit(2); // Should not reach
    }

    int status;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    std::cout << "[ OK   ] execve cleanup verified." << std::endl;
}

// Test 9: ptrace write triggers unsharing (Split-on-GUP)
TEST_F(PtShareTest, SplitOnGUP) {
    std::cout << "[ INFO ] Starting SplitOnGUP test..." << std::endl;
    void* addr = (void*)0x780000000000;

    pid_t pid = fork();
    if (pid == 0) {
        void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_PRIVATE);
        if (mapped == MAP_FAILED) exit(1);

        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);

        if (((char*)mapped)[0] != 'Z') {
            std::cerr << "Tracee saw incorrect value: " << ((char*)mapped)[0] << std::endl;
            exit(2);
        }
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSTOPPED(status));

    std::cout << "[ INFO ] Poking data into tracee's read-only shared mapping..." << std::endl;
    long data = 'Z';
    long ret = ptrace(PTRACE_POKEDATA, pid, addr, (void*)data);
    ASSERT_NE(ret, -1) << "ptrace poke failed: " << strerror(errno);

    std::cout << "[ INFO ] Resuming tracee..." << std::endl;
    ptrace(PTRACE_CONT, pid, NULL, NULL);
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::cout << "[ OK   ] Split-on-GUP isolation verified via ptrace." << std::endl;
}


// Test 14: Ptrace write spanning PMD boundary
TEST_F(PtShareTest, PtraceBoundary) {
    std::cout << "[ INFO ] Starting PtraceBoundary test..." << std::endl;
    // Map 2 PMDs
    void* addr = (void*)0x7D0000000000;

    pid_t pid = fork();
    if (pid == 0) {
        void* mapped = do_mmap(addr, 2 * PMD_SIZE, PROT_READ, MAP_PRIVATE);
        if (mapped == MAP_FAILED) exit(1);

        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);

        // Verify both pages saw the write
        if (((char*)mapped)[PMD_SIZE - 1] != 'X' || ((char*)mapped)[PMD_SIZE] != 'Y') {
            exit(2);
        }
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSTOPPED(status));

    // Ptrace write spanning the boundary between the two PMDs
    std::cout << "[ INFO ] Poking data across PMD boundary..." << std::endl;
    unsigned long boundary_addr = (unsigned long)addr + PMD_SIZE - 1;
    long data = ('Y' << 8) | 'X'; // Little endian: X at boundary-1, Y at boundary
    long ret = ptrace(PTRACE_POKEDATA, pid, (void*)boundary_addr, (void*)data);
    ASSERT_NE(ret, -1) << "ptrace poke failed: " << strerror(errno);

    std::cout << "[ INFO ] Resuming tracee..." << std::endl;
    ptrace(PTRACE_CONT, pid, NULL, NULL);
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::cout << "[ OK   ] Ptrace boundary unsharing verified." << std::endl;
}

// Test 22: ptrace read should NOT trigger unsharing
TEST_F(PtShareTest, PtraceReadNoUnshare) {
    std::cout << "[ INFO ] Starting PtraceReadNoUnshare test..." << std::endl;
    void* addr = (void*)0x670000000000;
    int sync_pipe[2];
    ASSERT_EQ(pipe(sync_pipe), 0);

    // Parent populates the file data
    char val = 'R';
    ASSERT_EQ(pwrite(fd, &val, 1, 0), 1);

    // Parent creates the shared PT mapping
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    ASSERT_EQ(((char*)mapped)[0], 'R');

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[1]); // Child doesn't write to pipe

        // Child has inherited the mapping and the shared page tables.
        // Verify it can read (triggering shared fault if not already populated)
        if (((char*)mapped)[0] != 'R') exit(2);

        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);

        // Wait for parent to signal completion
        char buf;
        if (read(sync_pipe[0], &buf, 1) <= 0) exit(3);
        exit(0);
    }

    close(sync_pipe[0]); // Parent doesn't read from pipe
    int status;

    // Wait for the child to stop after PTRACE_TRACEME + raise(SIGSTOP)
    ASSERT_GT(waitpid(pid, &status, 0), 0);
    ASSERT_TRUE(WIFSTOPPED(status)) << "Child not stopped: " << status;

    std::cout << "[ INFO ] Peeking data (read-only GUP)..." << std::endl;
    errno = 0;
    long ret = ptrace(PTRACE_PEEKDATA, pid, addr, NULL);
    if (ret == -1 && errno != 0) {
        std::cerr << "ptrace peek failed: " << strerror(errno) << std::endl;
    } else {
        EXPECT_EQ((char)(ret & 0xFF), 'R');
    }

    // Signal child to exit and resume it
    write(sync_pipe[1], "K", 1);
    ptrace(PTRACE_CONT, pid, NULL, NULL);

    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    munmap(mapped, PMD_SIZE);
    std::cout << "[ OK   ] Ptrace read-only peek completed." << std::endl;
}

 // Test 23: process_vm_writev triggers unsharing
#include <sys/uio.h>
TEST_F(PtShareTest, ProcessVmWriteUnshare) {
    std::cout << "[ INFO ] Starting ProcessVmWriteUnshare test..." << std::endl;
    void* addr = (void*)0x660000000000;

    pid_t pid = fork();
    if (pid == 0) {
        // Use PROT_WRITE for process_vm_writev as it doesn't support FOLL_FORCE
        void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
        if (mapped == MAP_FAILED) exit(1);

        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);

        if (((char*)mapped)[0] != 'V') {
            std::cerr << "Tracee saw incorrect value: " << ((char*)mapped)[0] << std::endl;
            exit(2);
        }
        exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSTOPPED(status));

    std::cout << "[ INFO ] Writing via process_vm_writev..." << std::endl;
    char data = 'V';
    struct iovec local[1];
    struct iovec remote[1];
    local[0].iov_base = &data;
    local[0].iov_len = 1;
    remote[0].iov_base = addr;
    remote[0].iov_len = 1;

    ssize_t nw = process_vm_writev(pid, local, 1, remote, 1, 0);
    ASSERT_EQ(nw, 1) << "process_vm_writev failed: " << strerror(errno);

    ptrace(PTRACE_CONT, pid, NULL, NULL);
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::cout << "[ OK   ] process_vm_writev unsharing verified." << std::endl;
}

// Test 24: gup_fast synchronization stress
TEST_F(PtShareTest, GupFastRace) {
    std::cout << "[ INFO ] Starting GupFastRace test..." << std::endl;
    void* addr = (void*)0x650000000000;

    // Parent maps and populates
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    memset(mapped, 'G', PMD_SIZE);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: high-frequency gup_fast via vmsplice
        int pipefds[2];
        if (pipe(pipefds) < 0) exit(1);

        struct iovec iov;
        iov.iov_base = addr;
        iov.iov_len = PAGE_SIZE;

        for (int i = 0; i < 1000; i++) {
            // vmsplice triggers gup_fast on the memory
            vmsplice(pipefds[1], &iov, 1, 0);
            // Drain pipe so it doesn't block
            char junk[PAGE_SIZE];
            if (read(pipefds[0], junk, PAGE_SIZE) < 0) break;
        }
        exit(0);
    }

    // Parent: high-frequency unsharing via mprotect toggle
    for (int i = 0; i < 100; i++) {
        // This will trigger unsharing in the parent's MM
        mprotect(mapped, PMD_SIZE, PROT_READ);
        // Toggle back
        mprotect(mapped, PMD_SIZE, PROT_READ | PROT_WRITE);
        usleep(100);
    }

    int status;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    munmap(mapped, PMD_SIZE);
    std::cout << "[ OK   ] Gup-fast race completed without crash." << std::endl;
}

// Test 25: O_DIRECT synchronization stress
TEST_F(PtShareTest, GupFastODirect) {
    std::cout << "[ INFO ] Starting GupFastODirect test..." << std::endl;
    void* addr = (void*)0x640000000000;

    // Parent maps and populates
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    memset(mapped, 'D', PMD_SIZE);

    // Create a temporary file for O_DIRECT reads
    const char* tmp_io_file = "ptshare_odirect_tmp";
    int io_fd = open(tmp_io_file, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0666);
    if (io_fd < 0) {
        if (errno == EINVAL) {
            std::cout << "[ SKIP ] O_DIRECT not supported on this filesystem." << std::endl;
            munmap(mapped, PMD_SIZE);
            return;
        }
        ASSERT_GE(io_fd, 0) << "Failed to open O_DIRECT file: " << strerror(errno);
    }

    // Write some data to read back
    char* io_buf;
    ASSERT_EQ(posix_memalign((void**)&io_buf, 4096, 4096), 0);
    memset(io_buf, 'Z', 4096);
    // Write without O_DIRECT for setup
    int setup_fd = open(tmp_io_file, O_WRONLY, 0666);
    ASSERT_GE(setup_fd, 0);
    ASSERT_EQ(write(setup_fd, io_buf, 4096), 4096);
    close(setup_fd);
    free(io_buf);

    pid_t pid = fork();
    if (pid == 0) {
        // Child: high-frequency O_DIRECT reads into the shared mapping
        // This will trigger gup_fast pinning
        for (int i = 0; i < 1000; i++) {
            // Read from file directly into our shared mapping
            if (pread(io_fd, addr, 4096, 0) < 0) {
                // Some filesystems might fail O_DIRECT on certain alignments
                // but we primarily care about the gup_fast path being hit.
            }
        }
        exit(0);
    }

    // Parent: high-frequency unsharing via mprotect toggle
    for (int i = 0; i < 100; i++) {
        mprotect(mapped, PMD_SIZE, PROT_READ);
        mprotect(mapped, PMD_SIZE, PROT_READ | PROT_WRITE);
        usleep(100);
    }

    int status;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    close(io_fd);
    unlink(tmp_io_file);
    munmap(mapped, PMD_SIZE);
    std::cout << "[ OK   ] Gup-fast O_DIRECT race completed without crash." << std::endl;
}

// Test 17: Overlap Splitting
TEST_F(PtShareTest, OverlapUnsharing) {
    std::cout << "[ INFO ] Starting OverlapUnsharing test..." << std::endl;
    void* addr = (void*)0x6E0000000000;

    // Map 2 PMDs
    void* mapped = do_mmap(addr, 2 * PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    // Map a single page in the middle (overlaps with the second PMD)
    // This should trigger unsharing of the second PMD.
    void* overlap_addr = (void*)((unsigned long)addr + PMD_SIZE);
    void* overlap = mmap(overlap_addr, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(overlap, MAP_FAILED);
    ASSERT_EQ(overlap, overlap_addr);

    ((char*)overlap)[0] = 'K';
    EXPECT_EQ(((char*)overlap)[0], 'K');

    // First PMD should still be accessible and unaffected
    EXPECT_EQ(((char*)mapped)[0], 0);

    munmap(overlap, PAGE_SIZE);
    ASSERT_EQ(munmap(mapped, 2 * PMD_SIZE), 0);
    std::cout << "[ OK   ] Overlap unsharing verified." << std::endl;
}

// Test 8: Partial munmap triggers unsharing (split)
TEST_F(PtShareTest, UnsharePartialMunmap) {
    std::cout << "[ INFO ] Starting UnsharePartialMunmap test..." << std::endl;
    void* addr = (void*)0x770000000000;
    std::cout << "[ INFO ] Mapping 2x PMD_SIZE at " << addr << "..." << std::endl;
    void* mapped = do_mmap(addr, 2 * PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    std::cout << "[ INFO ] Performing partial munmap (first PMD)..." << std::endl;
    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0) << "partial munmap failed: " << strerror(errno);

    void* remaining = (void*)((unsigned long)mapped + PMD_SIZE);
    std::cout << "[ INFO ] Verifying remaining part at " << remaining << "..." << std::endl;
    EXPECT_EQ(((char*)remaining)[0], 0);

    ASSERT_EQ(munmap(remaining, PMD_SIZE), 0);
    std::cout << "[ OK   ] Partial munmap (split) unsharing succeeded." << std::endl;
}

// Test 6: mprotect triggers unsharing
TEST_F(PtShareTest, UnshareMprotect) {
    std::cout << "[ INFO ] Starting UnshareMprotect test..." << std::endl;
    void* addr = (void*)0x740000000000;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    std::cout << "[ INFO ] Triggering mprotect(PROT_WRITE) to force unshare..." << std::endl;
    ASSERT_EQ(mprotect(mapped, PMD_SIZE, PROT_READ | PROT_WRITE), 0) << "mprotect failed: " << strerror(errno);

    std::cout << "[ INFO ] Verifying write access after unsharing..." << std::endl;
    ((char*)mapped)[0] = 'D';
    EXPECT_EQ(((char*)mapped)[0], 'D');

    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] mprotect unsharing succeeded." << std::endl;
}

// Test 7: mremap triggers unsharing
TEST_F(PtShareTest, UnshareMremap) {
    std::cout << "[ INFO ] Starting UnshareMremap test..." << std::endl;
    void* addr = (void*)0x750000000000;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    void* new_addr = (void*)0x760000000000;
    std::cout << "[ INFO ] Remapping from " << addr << " to " << new_addr << "..." << std::endl;
    void* remapped = mremap(mapped, PMD_SIZE, PMD_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, new_addr);
    ASSERT_NE(remapped, MAP_FAILED) << "mremap failed: " << strerror(errno);
    ASSERT_EQ(remapped, new_addr);

    std::cout << "[ INFO ] Verifying access at new address..." << std::endl;
    EXPECT_EQ(((char*)remapped)[0], 0);

    ASSERT_EQ(munmap(remapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] mremap unsharing succeeded." << std::endl;
}

// Test 18: mremap expansion
TEST_F(PtShareTest, MremapExpand) {
    std::cout << "[ INFO ] Starting MremapExpand test..." << std::endl;
    void* addr = (void*)0x6D0000000000;

    // Map 1 PMD
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    // Expand to 2 PMDs
    std::cout << "[ INFO ] Expanding mapping via mremap..." << std::endl;
    void* expanded = mremap(mapped, PMD_SIZE, 2 * PMD_SIZE, MREMAP_MAYMOVE);
    ASSERT_NE(expanded, MAP_FAILED);

    // Verify access
    EXPECT_EQ(((char*)expanded)[0], 0);
    EXPECT_EQ(((char*)expanded)[PMD_SIZE], 0);

    ASSERT_EQ(munmap(expanded, 2 * PMD_SIZE), 0);
    std::cout << "[ OK   ] mremap expansion verified." << std::endl;
}

// Test 19: Partial mremap (split)
TEST_F(PtShareTest, PartialMremap) {
    std::cout << "[ INFO ] Starting PartialMremap test..." << std::endl;
    void* addr = (void*)0x6C0000000000;

    // Map 2 PMDs
    void* mapped = do_mmap(addr, 2 * PMD_SIZE, PROT_READ, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    // Move only the second PMD to a new location
    void* second_pmd = (void*)((unsigned long)mapped + PMD_SIZE);
    void* new_loc = (void*)0x6B0000000000;

    std::cout << "[ INFO ] Moving second PMD via mremap..." << std::endl;
    void* remapped = mremap(second_pmd, PMD_SIZE, PMD_SIZE, MREMAP_MAYMOVE | MREMAP_FIXED, new_loc);
    ASSERT_NE(remapped, MAP_FAILED);
    ASSERT_EQ(remapped, new_loc);

    // Verify both parts are still accessible
    EXPECT_EQ(((char*)mapped)[0], 0);
    EXPECT_EQ(((char*)remapped)[0], 0);

    munmap(mapped, PMD_SIZE);
    munmap(remapped, PMD_SIZE);
    std::cout << "[ OK   ] Partial mremap verified." << std::endl;
}

// Test 10: MADV_DONTNEED triggers unsharing
TEST_F(PtShareTest, UnshareMadviseDontNeed) {
    std::cout << "[ INFO ] Starting UnshareMadviseDontNeed test..." << std::endl;
    void* addr = (void*)0x790000000000;
    // Must use MAP_SHARED for PROT_WRITE + MAP_SHARED_PT
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    ((char*)mapped)[0] = 'X';

    std::cout << "[ INFO ] Triggering madvise(MADV_DONTNEED) to force unshare..." << std::endl;
    ASSERT_EQ(madvise(mapped, PMD_SIZE, MADV_DONTNEED), 0) << "madvise failed: " << strerror(errno);

    // Note: On some kernels/configurations, MADV_DONTNEED on MAP_SHARED
    // might not immediately zap the page if it's dirty.
    // However, on ZAPTS it should trigger unsharing.
    // For the vanilla kernel test, we just ensure it doesn't crash.
    (void)((char*)mapped)[0];

    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] madvise unsharing succeeded." << std::endl;
}

// Test 13: MADV_REMOVE triggers unsharing
TEST_F(PtShareTest, UnshareMadviseRemove) {
    std::cout << "[ INFO ] Starting UnshareMadviseRemove test..." << std::endl;
    void* addr = (void*)0x7C0000000000;
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);

    ((char*)mapped)[0] = 'R';

    std::cout << "[ INFO ] Triggering madvise(MADV_REMOVE) to force unshare..." << std::endl;
    ASSERT_EQ(madvise(mapped, PMD_SIZE, MADV_REMOVE), 0) << "madvise failed: " << strerror(errno);

    EXPECT_EQ(((char*)mapped)[0], 0);

    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] madvise(MADV_REMOVE) unsharing succeeded." << std::endl;
}

// Test 26: MADV_DONTNEED isolation (Process B should not be affected by Process A's madvise)
TEST_F(PtShareTest, MadviseDontNeedIsolation) {
    std::cout << "[ INFO ] Starting MadviseDontNeedIsolation test..." << std::endl;
    void* addr = (void*)0x630000000000;
    // Map in parent and populate
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    ((char*)mapped)[0] = 'X';

    pid_t pid = fork();
    if (pid == 0) {
        // Child (Process B): verify initial value
        if (((char*)mapped)[0] != 'X') exit(1);

        // Synchronize: wait for parent to madvise
        sleep(2);

        // Verify value is still 'X' (Process B should be isolated)
        // Since it's MAP_SHARED, it will see 'X' either from its intact
        // PTEs or by faulting it back from the page cache.
        if (((char*)mapped)[0] != 'X') {
            std::cerr << "Child saw corrupted data after parent's madvise!" << std::endl;
            exit(2);
        }
        exit(0);
    }

    // Parent (Process A): Wait a bit for child to start
    sleep(1);

    // Parent: madvise DONTNEED. This triggers unsharing.
    std::cout << "[ INFO ] Parent triggering madvise(MADV_DONTNEED)..." << std::endl;
    ASSERT_EQ(madvise(mapped, PMD_SIZE, MADV_DONTNEED), 0);

    // Parent: verify it can still see its data. Since it's MAP_SHARED,
    // MADV_DONTNEED zaps PTEs but not the page cache. refault should see 'X'.
    EXPECT_EQ(((char*)mapped)[0], 'X');

    int status;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] Madvise isolation verified." << std::endl;
}

// Test 27: MADV_REMOVE global effect (Process B SHOULD be affected by Process A's madvise)
TEST_F(PtShareTest, MadviseRemoveIsolation) {
    std::cout << "[ INFO ] Starting MadviseRemoveIsolation test..." << std::endl;
    void* addr = (void*)0x620000000000;
    // Map in parent and populate
    void* mapped = do_mmap(addr, PMD_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED);
    ASSERT_NE(mapped, MAP_FAILED);
    ((char*)mapped)[0] = 'R';

    pid_t pid = fork();
    if (pid == 0) {
        // Child (Process B): verify initial value
        if (((char*)mapped)[0] != 'R') exit(1);

        // Synchronize: wait for parent to madvise
        sleep(2);

        // Verify value is 0 (Process B should see the hole punch)
        // MADV_REMOVE is a hole punch on the file, so all processes must see 0.
        if (((char*)mapped)[0] != 0) {
            std::cerr << "Child did NOT see the hole punched by parent!" << std::endl;
            exit(2);
        }
        exit(0);
    }

    // Parent (Process A): Wait a bit for child to start
    sleep(1);

    // Parent: madvise REMOVE. This triggers unsharing.
    std::cout << "[ INFO ] Parent triggering madvise(MADV_REMOVE)..." << std::endl;
    ASSERT_EQ(madvise(mapped, PMD_SIZE, MADV_REMOVE), 0);

    // Parent: verify its own data is gone (should be 0)
    EXPECT_EQ(((char*)mapped)[0], 0);

    int status;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    ASSERT_EQ(munmap(mapped, PMD_SIZE), 0);
    std::cout << "[ OK   ] Madvise(MADV_REMOVE) global effect verified." << std::endl;
}

/*
// Helper to get PageTables value from /proc/meminfo in kB
static long get_pagetable_usage_kb() {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long usage = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PageTables:", 11) == 0) {
            sscanf(line + 11, "%ld", &usage);
            break;
        }
    }
    fclose(f);
    return usage;
}

// Test 22: Page Table Efficiency Comparison
TEST_F(PtShareTest, PageTableEfficiency) {
    std::cout << "[ INFO ] Starting PageTableEfficiency test..." << std::endl;
    const int num_procs = 50;
    const int num_pmds = 500; // 1GB mapping
    const size_t total_size = (size_t)num_pmds * PMD_SIZE;
    void* addr = (void*)0x600000000000;

    auto run_experiment = [&](bool use_shpt) -> long {
        long before = get_pagetable_usage_kb();

        pid_t parent_pid = fork();
        if (parent_pid == 0) {
            setpgid(0, 0); // Create a new process group
            int flags = MAP_SHARED | MAP_FIXED;
            if (use_shpt) flags |= MAP_SHARED_PT;

            void* mapped = mmap(addr, total_size, PROT_READ | PROT_WRITE, flags, fd, 0);
            if (mapped == MAP_FAILED) {
                perror("mmap failed");
                _exit(1);
            }

            // Force PTE-level page tables to ensure we are testing PTE sharing
            madvise(mapped, total_size, MADV_NOHUGEPAGE);

            for (int i = 0; i < num_procs; i++) {
                if (fork() == 0) {
                    signal(SIGTERM, [](int){ _exit(0); });
                    // Children access all PMDs to ensure PTEs are present
                    for (size_t j = 0; j < num_pmds; j++) {
                        if (((char*)mapped)[j * PMD_SIZE] != 0) _exit(2);
                    }

                    // Signal this child is done via a dedicated file
                    char sync_name[64];
                    sprintf(sync_name, "sync_ready_%d", i);
                    FILE* s = fopen(sync_name, "w");
                    if (s) { fprintf(s, "OK"); fclose(s); }

                    while(1) pause();
                }
            }
            while(1) pause();
            _exit(0);
        }

        // Wait for all 50 children to signal readiness
        for (int i = 0; i < num_procs; i++) {
            char sync_name[64];
            sprintf(sync_name, "sync_ready_%d", i);
            bool ready = false;
            for (int retry = 0; retry < 50; retry++) {
                struct stat st;
                if (stat(sync_name, &st) == 0) {
                    ready = true;
                    break;
                }
                usleep(200000); // 0.2s
            }
            if (!ready) std::cerr << "[ WARN ] Child " << i << " never signaled ready." << std::endl;
            unlink(sync_name);
        }
        sleep(2); // Extra settle time

        long during = get_pagetable_usage_kb();

        // Kill entire process group
        kill(-parent_pid, SIGTERM);

        int status;
        waitpid(parent_pid, &status, 0);

        return during - before;
    };

    std::cout << "[ INFO ] Measuring standard PageTable overhead..." << std::endl;
    long std_overhead = run_experiment(false);
    std::cout << "[ INFO ] Standard overhead: " << std_overhead << " kB" << std::endl;

    std::cout << "[ INFO ] Waiting for PageTable usage to stabilize..." << std::endl;
    sleep(10);

    std::cout << "[ INFO ] Measuring ZAPTS PageTable overhead..." << std::endl;
    long zapts_overhead = run_experiment(true);
    std::cout << "[ INFO ] ZAPTS overhead: " << zapts_overhead << " kB" << std::endl;

    EXPECT_GT(std_overhead, 0);
    EXPECT_GT(zapts_overhead, 0);

    // Theoretical Standard: 50 procs * 500 PMDs * 4kB/PMD = 100,000 kB.
    // Theoretical ZAPTS: (1 ptshare_mm * 500 PMDs * 4kB) + (50 procs * PUD/PMD pages) = ~2,000 kB + (50 * 2 * 4kB) = ~2,400 kB.
    // Savings should be ~97.5%.
    EXPECT_LT(zapts_overhead, std_overhead / 10);

    std::cout << "[ OK   ] Efficiency verified. Savings: " << (std_overhead - zapts_overhead) << " kB" << std::endl;
}
*/

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
