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

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
