Write a Linux kernel char driver module named scull_exam, strongly inspired by LDD3 scull/scullp/scullv, with naming and data layout kept as close as reasonably possible to the book.

Goal:
Implement one module exposing 3 families of devices:
- /dev/scullp0..3  : backend based on __get_free_pages/free_pages
- /dev/scullv0..3  : backend based on vmalloc/vfree
- /dev/scullm0..3  : backend where struct scull_qset objects are allocated from a mempool created with mempool_create(..., mempool_alloc_slab, mempool_free_slab, cache), while payload quantums are still allocated page-wise with __get_free_pages/free_pages

Important design constraints:
1. Keep classic scull naming wherever possible:
   - struct scull_qset
   - struct scull_dev
   - scull_trim
   - scull_follow
   - scull_open
   - scull_release
   - scull_read
   - scull_write
   - scull_llseek
   - scull_setup_cdev
2. Preserve classic scull data model:
   - struct scull_qset { void **data; struct scull_qset *next; }
   - struct scull_dev stores:
     * struct scull_qset *data
     * int quantum
     * int qset
     * unsigned long size
     * unsigned int access_key
     * lock
     * struct cdev cdev
   - backend-specific fields may be appended at the end
3. Use one shared implementation of read/write/follow/trim for all device families.
4. Backend differences must be isolated in helpers:
   - scull_alloc_quantum(dev)
   - scull_free_quantum(dev, ptr)
   - scull_alloc_qset(dev)
   - scull_free_qset(dev, qs)
5. Do not over-engineer. No xarray, no kvmalloc substitutions, no redesign of scull storage model.
6. No mmap/ioctl/proc interface in the first version.
7. Produce code that is easy to defend in an exam.

Backend details:
- pages backend:
  quantum = PAGE_SIZE << order
  allocation = __get_free_pages(GFP_KERNEL, order)
  free = free_pages((unsigned long)ptr, order)
  zero fill after allocation with memset
- vmalloc backend:
  quantum = PAGE_SIZE << order
  allocation = vmalloc(PAGE_SIZE << order)
  free = vfree(ptr)
  zero fill after allocation with memset
- mempool backend:
  qset nodes only must come from mempool
  create slab cache for struct scull_qset
  create mempool with mempool_create(scullm_pool_min_nr, mempool_alloc_slab, mempool_free_slab, cache)
  payload quantums stay page-based
  qset->data array may be allocated with kcalloc

Minor layout:
- minors 0..3   => scullp0..3
- minors 4..7   => scullv0..3
- minors 8..11  => scullm0..3

Global/module parameters:
- scull_major
- scull_minor
- scull_nr_devs = 4
- scull_qset = 1000
- scullp_order = 0
- scullv_order = 4
- scullm_order = 0
- scullm_pool_min_nr = 32

Locking:
- Prefer struct mutex for modern kernels, but keep comments explaining that original LDD3 used struct semaphore sem.
- Make the code compile on a modern kernel.
- Use lock/unlock consistently around read/write/trim paths.

Open behavior:
- In open(), use container_of(inode->i_cdev, struct scull_dev, cdev)
- store dev into filp->private_data
- if opened write-only, call scull_trim(dev), just like classic scull

Storage math:
Keep the classic scull addressing scheme:
- itemsize = quantum * qset
- item = *f_pos / itemsize
- rest = *f_pos % itemsize
- s_pos = rest / quantum
- q_pos = rest % quantum

Implementation requirements:
- Provide scull_exam.h and scull_exam.c and Makefile
- Register cdevs and create device nodes via class/device_create
- Handle all cleanup paths correctly
- No memory leaks on partial initialization failure
- Every helper should be small and readable
- Put comments that explain why each field exists, especially order, quantum, qset, size, backend, mempool cache/pool

Output format:
1. First give a brief architecture summary
2. Then provide full code for:
   - scull_exam.h
   - scull_exam.c
   - Makefile
3. Then provide a short explanation of each struct field and each global variable
4. Then provide build/load/test commands:
   - make
   - insmod
   - ls /dev/scull*
   - echo/cat tests
   - rmmod
5. Then provide a section called “Exam explanation notes” explaining how scullp, scullv, and mempool differ conceptually

Code quality expectations:
- Clear error handling
- Symmetric allocation/free
- Minimal duplication
- LDD3-like spirit over modern abstraction cleverness