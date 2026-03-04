/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCULL_EXAM_H_
#define _SCULL_EXAM_H_

#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/mempool.h>

/* ---------- classic scull linked-list node (LDD3 §3) ---------- */
struct scull_qset {
	void **data;            /* array of <qset> pointers to quantum buffers */
	struct scull_qset *next;
};

/* ---------- per-device structure (mirrors LDD3 scull_dev) ---------- */
struct scull_dev {
	struct scull_qset *data; /* head of the qset linked list              */
	int quantum;             /* size in bytes of each quantum buffer       */
	int qset;                /* number of quantum pointers per qset node   */
	unsigned long size;      /* total amount of data currently stored      */
	unsigned int access_key; /* unused here, kept for LDD3 compatibility   */
	/*
	 * LDD3 used  struct semaphore sem;
	 * Modern kernels prefer struct mutex for sleepable critical sections.
	 */
	struct mutex lock;
	struct cdev cdev;        /* embedded char device structure              */

	/* ---- backend-specific fields (appended per spec) ---- */
	int order;               /* page order: quantum = PAGE_SIZE << order    */

	/* function pointers that isolate allocation strategy */
	void *(*alloc_quantum)(struct scull_dev *);
	void  (*free_quantum)(struct scull_dev *, void *);
	struct scull_qset *(*alloc_qset)(struct scull_dev *);
	void  (*free_qset)(struct scull_dev *, struct scull_qset *);

	/* mempool backend only */
	struct kmem_cache *qset_cache; /* slab cache for struct scull_qset      */
	mempool_t         *qset_pool;  /* mempool wrapping qset_cache           */
};

/* module parameters (declared in .c, externed here for clarity) */
extern int scull_major;
extern int scull_minor;
extern int scull_nr_devs;
extern int scull_p_qset;       /* renamed to avoid clash with struct field */
extern int scullp_order;
extern int scullv_order;
extern int scullm_order;
extern int scullm_pool_min_nr;

#define SCULL_NR_DEVS   4      /* devices per family                       */
#define SCULL_FAMILIES  3      /* scullp, scullv, scullm                   */
#define SCULL_TOTAL     (SCULL_NR_DEVS * SCULL_FAMILIES)

#endif /* _SCULL_EXAM_H_ */
