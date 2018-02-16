# Kernel page table isolation

## Introduction

This document only applies to Intel x86 CPUs.

Due to a widespread hardware vulnerability on Intel CPUs ([Meltdown]), it is
possible to read from kernel memory via cache timing side-channels (in
particular, bypassing the user/supervisor bit in the page table entries).  To
mitigate this, we need to ensure that any memory that we wish to keep private
from a process is not present in its page tables.

Our implementation of this mitigation is largely based on the [KAISER] paper.

Each user process is given two top-level page tables (PML4s) which we call the
uPML4 and kPML4.  These are allocated as an 8KB-aligned 8KB block of memory.
The kPML4 is always the lower half of the block (i.e. bit 12 is always 0) for
it.

## kPML4/uPML4 Mirroring

The kPML4 is the copy that is acted on by all page table modification
operations.  Whenever an entry in the low half of the kPML4 changes (all
usermode addresses are in the low 47-bits of the address space), the change
is mirrored to the uPML4.

[Meltdown]: https://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2017-5754
[KAISER]: https://gruss.cc/files/kaiser.pdf
