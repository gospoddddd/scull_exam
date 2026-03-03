/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCULL_EXAM_H_
#define _SCULL_EXAM_H_

#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/mempool.h>

/* ---------- классический узел связного списка scull (LDD3 §3) ---------- */
struct scull_qset {
	void **data;            /* массив из <qset> указателей на квантовые буферы */
	struct scull_qset *next;
};

/* ---------- структура устройства (повторяет LDD3 scull_dev) ---------- */
struct scull_dev {
	struct scull_qset *data; /* голова связного списка qset                */
	int quantum;             /* размер каждого квантового буфера в байтах  */
	int qset;                /* число указателей на кванты в одном узле     */
	unsigned long size;      /* общий объём данных, хранимых сейчас         */
	unsigned int access_key; /* здесь не используется, оставлено для LDD3   */
	/*
	 * В LDD3 использовался struct semaphore sem;
	 * В современных ядрах для «спящих» критических секций обычно используют mutex.
	 */
	struct mutex lock;
	struct cdev cdev;        /* встроенная структура символьного устройства */

	/* ---- поля, специфичные для backend (добавлены по заданию) ---- */
	int order;               /* порядок страниц: quantum = PAGE_SIZE << order */

	/* указатели на функции, изолирующие стратегию аллокации */
	void *(*alloc_quantum)(struct scull_dev *);
	void  (*free_quantum)(struct scull_dev *, void *);
	struct scull_qset *(*alloc_qset)(struct scull_dev *);
	void  (*free_qset)(struct scull_dev *, struct scull_qset *);

	/* только для backend на mempool */
	struct kmem_cache *qset_cache; /* slab-кэш для struct scull_qset          */
	mempool_t         *qset_pool;  /* mempool поверх qset_cache               */
};

/* параметры модуля (объявлены в .c, вынесены extern здесь для ясности) */
extern int scull_major;
extern int scull_minor;
extern int scull_nr_devs;
extern int scull_p_qset;       /* переименовано, чтобы не конфликтовать с полем структуры */
extern int scullp_order;
extern int scullv_order;
extern int scullm_order;
extern int scullm_pool_min_nr;

#define SCULL_NR_DEVS   4      /* устройств в каждом семействе             */
#define SCULL_FAMILIES  3      /* scullp, scullv, scullm                   */
#define SCULL_TOTAL     (SCULL_NR_DEVS * SCULL_FAMILIES)

#endif /* _SCULL_EXAM_H_ */
