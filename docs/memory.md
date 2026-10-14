# Virtual memory

Lab: [`mem_lab`](../labs/memory/mem_lab.c). Tools: `llps -p PID` (VmSize, VmRSS, RssAnon,
RssFile from `/proc/<pid>/status`). Measurements: `bench_fileio`, `bench_spawn`.

## Allocation is lazy

```text
== 1. allocation is lazy ==        (mem_lab 256)
  start                              VmSize     2840 kB   VmRSS     1760 kB
  after malloc                       VmSize   264988 kB   VmRSS     1956 kB
  after writing the first half       VmSize   264988 kB   VmRSS   133028 kB
  after writing everything           VmSize   264988 kB   VmRSS   264100 kB
  page faults: first touch of 128 MB: 32768, second touch of all 256 MB: 0
  after free                         VmSize     2840 kB   VmRSS     1952 kB
```

* `malloc(256 MB)` only reserves **address space** (VmSize). No physical memory is used yet.
* Every first write to a 4 KiB page triggers a **page fault**. The kernel allocates a physical
  page, zeroes it and maps it. 128 MB / 4 KiB = **32,768 faults, exactly as measured**.
* The second pass causes **0** faults, because the pages are already mapped.
* `free()` of a large block unmaps it immediately and both numbers drop back.

This is also why Linux can *overcommit*: the sum of all VmSizes can exceed RAM + swap, because
most reserved memory is never touched.

**A lesson from building this lab.** The first version printed identical numbers before and after
`malloc`. GCC had noticed that the buffer was never read and deleted the `malloc`, the `memset`
and the `free` altogether. `escape()` in [`common/util.h`](../common/util.h) (an empty `asm`
statement with a memory clobber) now tells the compiler the pointer is used.

## Where malloc gets memory

```text
  malloc(64)         0x631d30e4a020  in  631d30e49000-631d30e6b000 [heap]
  malloc(4 MB)       0x723df03ff010  in  723df03ff000-723df0800000 [anonymous mapping]
  stack variable     0x7ffe1242e8f4  in  7ffe1240f000-7ffe12430000 [stack]
  a function's code  0x631d19d699e0  in  631d19d69000-631d19d6b000 /home/mouad/linuxlab/build/bin/mem_lab
```

glibc serves small requests from the `[heap]`, which it grows with `brk()`. Requests of at least
128 KiB (`M_MMAP_THRESHOLD`) get their own `mmap()`, and `free()` can return that mapping to the
kernel at once. Heap memory can only be returned from the top.

## Mapping a file

```text
  mmap(file)         0x723def800000  in  723def800000-723df0800000 /tmp/linuxlab-mem.Dz03Zl
  /proc/self/maps: 27 lines before, 28 after
  before reading: RssFile   1844 kB  RssShmem      0 kB
  after  reading: RssFile   1844 kB  RssShmem  16384 kB
```

Reading a mapped file maps **page-cache pages** into the process. No copy is made; that is the
source of `mmap`'s 2× advantage over `read()` in `bench_fileio`. Here `/tmp` is a tmpfs, so the
pages count as shared memory (RssShmem); on ext4 they would appear as RssFile. They never count
as RssAnon, because the kernel can drop them and re-read the file.

## fork() and copy-on-write

```text
  child read  16384 pages:      0 page faults
  child wrote 16384 pages:  16386 page faults, 53.4 ms (3.26 us per copied page)
  parent still sees p[0] = 1
```

After `fork()` parent and child share every page read-only. A **write** triggers a fault, and
the kernel copies that one page, which cost 3.3 µs per page here. Reads never copy. The cost
that `fork()` itself pays up front is copying the *page tables*; `bench_spawn` shows it growing
from 0.3 ms to 7.9 ms as the parent grows to 256 MB
([experiments.md §1](experiments.md#1-process-creation-fork-gets-slower-as-the-parent-grows)).

The lab disables transparent huge pages for this region (`madvise(MADV_NOHUGEPAGE)`), so one
fault equals one 4 KiB page. With THP a single fault can map 2 MiB at once.
