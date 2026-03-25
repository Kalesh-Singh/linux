# Shared Page Table Test Suite

This directory contains userspace tests to verify the functionality of the shared page table (`ptshare_mm`) implementation.

## Prerequisites
- Linux kernel built with `CONFIG_SHARED_PAGETABLE=y`.
- `g++` (C++17 support).
- Google Test (`gtest`).

### Installing Google Test (Ubuntu/Debian)
You can install the Google Test development files using:
```bash
sudo apt-get update
sudo apt-get install libgtest-dev
```
On some distributions, you may need to compile the source provided by the package:
```bash
cd /usr/src/gtest
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib
```

## How to Run
The Makefile is configured to statically link all dependencies (including `gtest` and `libstdc++`), resulting in a portable binary.

*Note: You may see a warning about `getaddrinfo` during the build (`Using 'getaddrinfo' in statically linked applications...`). This is a known glibc behavior when statically linking and can be safely ignored as these tests do not use networking.*

```bash
cd tests
make
./ptshare_test
```

## Test Cases
1.  **BasicMapping**: Verifies that `mmap()` with `MAP_SHARED_PT` succeeds when address and size are PMD-aligned.
2.  **AlignmentFailureAddress**: Verifies that `mmap()` with `MAP_SHARED_PT` fails with `EINVAL` if the address is not PMD-aligned.
3.  **AlignmentFailureSize**: Verifies that `mmap()` with `MAP_SHARED_PT` fails with `EINVAL` if the size is not a multiple of PMD_SIZE.
4.  **FaultSharing**: Verifies that two independent processes mapping the same file with `MAP_SHARED_PT` share the same data, confirming PTE splicing.
5.  **ForkInheritance**: Verifies that a child process correctly inherits a `VM_SHARED_PT` mapping from its parent.
6.  **UnshareMprotect**: Verifies that calling `mprotect()` on a shared mapping triggers a transparent unshare, converting it to a private mapping.
7.  **UnshareMremap**: Verifies that calling `mremap()` on a shared mapping triggers a transparent unshare.
8.  **UnsharePartialMunmap**: Verifies that unmapping only a portion of a shared VMA triggers a transparent unshare before the split.
9.  **SplitOnGUP**: Verifies that a `ptrace` write (using `FOLL_WRITE` GUP) to a read-only shared mapping triggers a "Split-on-GUP" operation, isolating the change to the tracee.
