// SPDX-License-Identifier: GPL-2.0
/*
 * scull_exam — three-backend char-device driver inspired by LDD3 scull/scullp/scullv.
 *
 * Families:
 *   scullp0..3  — quantums via __get_free_pages / free_pages
 *   scullv0..3  — quantums via vmalloc / vfree
 *   scullm0..3  — qset nodes from mempool (slab-backed), quantums page-based
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/mempool.h>
#include <linux/string.h>

#include "scull_exam.h"

/* ================================================================
 * Module parameters
 * ================================================================ */

int scull_major;                   /* 0 => dynamic allocation        */
int scull_minor;                   /* first minor number             */
int scull_nr_devs   = SCULL_NR_DEVS; /* devices per family           */
int scull_p_qset    = 1000;        /* default qset (pointers/node)   */
int scullp_order    = 0;           /* page order for scullp          */
int scullv_order    = 4;           /* page order for scullv          */
int scullm_order    = 0;           /* page order for scullm quantums */
int scullm_pool_min_nr = 32;       /* mempool minimum reserved objs  */

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_p_qset, int, S_IRUGO);
module_param(scullp_order, int, S_IRUGO);
module_param(scullv_order, int, S_IRUGO);
module_param(scullm_order, int, S_IRUGO);
module_param(scullm_pool_min_nr, int, S_IRUGO);

/* ================================================================
 * Globals
 * ================================================================ */

static struct scull_dev *scull_devices;  /* array of SCULL_TOTAL devices */
static struct class     *scull_class;    /* sysfs class for udev nodes   */
static dev_t             scull_devno;    /* first dev_t (major+minor)    */

/* device name prefixes, indexed by family */
static const char * const family_name[] = { "scullp", "scullv", "scullm" };

/* ================================================================
 * Backend helpers — pages (scullp)
 * ================================================================ */

static void *scullp_alloc_quantum(struct scull_dev *dev)
{
	void *p = (void *)__get_free_pages(GFP_KERNEL, dev->order);
	if (p)
		memset(p, 0, PAGE_SIZE << dev->order);
	return p;
}

static void scullp_free_quantum(struct scull_dev *dev, void *ptr)
{
	free_pages((unsigned long)ptr, dev->order);
}

static struct scull_qset *scullp_alloc_qset(struct scull_dev *dev)
{
	return kzalloc(sizeof(struct scull_qset), GFP_KERNEL);
}

static void scullp_free_qset(struct scull_dev *dev, struct scull_qset *qs)
{
	kfree(qs);
}

/* ================================================================
 * Backend helpers — vmalloc (scullv)
 * ================================================================ */

static void *scullv_alloc_quantum(struct scull_dev *dev)
{
	void *p = vmalloc(PAGE_SIZE << dev->order);
	if (p)
		memset(p, 0, PAGE_SIZE << dev->order);
	return p;
}

static void scullv_free_quantum(struct scull_dev *dev, void *ptr)
{
	vfree(ptr);
}

static struct scull_qset *scullv_alloc_qset(struct scull_dev *dev)
{
	return kzalloc(sizeof(struct scull_qset), GFP_KERNEL);
}

static void scullv_free_qset(struct scull_dev *dev, struct scull_qset *qs)
{
	kfree(qs);
}

/* ================================================================
 * Backend helpers — mempool (scullm)
 * ================================================================ */

static void *scullm_alloc_quantum(struct scull_dev *dev)
{
	/* quantums stay page-based, same as scullp */
	void *p = (void *)__get_free_pages(GFP_KERNEL, dev->order);
	if (p)
		memset(p, 0, PAGE_SIZE << dev->order);
	return p;
}

static void scullm_free_quantum(struct scull_dev *dev, void *ptr)
{
	free_pages((unsigned long)ptr, dev->order);
}

static struct scull_qset *scullm_alloc_qset(struct scull_dev *dev)
{
	/* allocate from the mempool (which draws from slab cache) */
	struct scull_qset *qs = mempool_alloc(dev->qset_pool, GFP_KERNEL);
	if (qs)
		memset(qs, 0, sizeof(*qs));
	return qs;
}

static void scullm_free_qset(struct scull_dev *dev, struct scull_qset *qs)
{
	mempool_free(qs, dev->qset_pool);
}

/* ================================================================
 * Shared core — scull_trim / scull_follow
 * ================================================================ */

/*
 * scull_trim — release all storage held by a device.
 * Caller must hold dev->lock.
 */
static int scull_trim(struct scull_dev *dev)
{
	struct scull_qset *next, *dptr;
	int qset = dev->qset;
	int i;

	for (dptr = dev->data; dptr; dptr = next) {
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					dev->free_quantum(dev, dptr->data[i]);
			kfree(dptr->data);
			dptr->data = NULL;
		}
		next = dptr->next;
		dev->free_qset(dev, dptr);
	}
	dev->size = 0;
	dev->data = NULL;
	return 0;
}

/*
 * scull_follow — walk (and optionally create) the qset linked list
 * to reach item number <n>.  Returns NULL on allocation failure.
 */
static struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
	struct scull_qset *qs = dev->data;

	if (!qs) {
		qs = dev->alloc_qset(dev);
		if (!qs)
			return NULL;
		dev->data = qs;
	}

	while (n--) {
		if (!qs->next) {
			qs->next = dev->alloc_qset(dev);
			if (!qs->next)
				return NULL;
		}
		qs = qs->next;
	}
	return qs;
}

/* ================================================================
 * File operations
 * ================================================================ */

static int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	/* if opened write-only, truncate (classic scull behaviour) */
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		mutex_lock(&dev->lock);
		scull_trim(dev);
		mutex_unlock(&dev->lock);
	}
	return 0;
}

static int scull_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t scull_read(struct file *filp, char __user *buf, size_t count,
			  loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum  = dev->quantum;
	int qset     = dev->qset;
	int itemsize = quantum * qset;  /* bytes per linked-list node */
	int item, s_pos, q_pos, rest;
	ssize_t retval = 0;

	mutex_lock(&dev->lock);

	if (*f_pos >= dev->size)
		goto out;
	if (*f_pos + count > dev->size)
		count = dev->size - *f_pos;

	item  = (long)*f_pos / itemsize;
	rest  = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item);

	if (!dptr || !dptr->data || !dptr->data[s_pos])
		goto out;

	/* read only up to end of this quantum */
	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_to_user(buf, dptr->data[s_pos] + q_pos, count)) {
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;

out:
	mutex_unlock(&dev->lock);
	return retval;
}

static ssize_t scull_write(struct file *filp, const char __user *buf,
			   size_t count, loff_t *f_pos)
{
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum  = dev->quantum;
	int qset     = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval = -ENOMEM;

	mutex_lock(&dev->lock);

	item  = (long)*f_pos / itemsize;
	rest  = (long)*f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item);
	if (!dptr)
		goto out;

	if (!dptr->data) {
		dptr->data = kcalloc(qset, sizeof(void *), GFP_KERNEL);
		if (!dptr->data)
			goto out;
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = dev->alloc_quantum(dev);
		if (!dptr->data[s_pos])
			goto out;
	}

	/* write only up to end of this quantum */
	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;

	if (dev->size < *f_pos)
		dev->size = *f_pos;

out:
	mutex_unlock(&dev->lock);
	return retval;
}

static loff_t scull_llseek(struct file *filp, loff_t off, int whence)
{
	struct scull_dev *dev = filp->private_data;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = off;
		break;
	case SEEK_CUR:
		newpos = filp->f_pos + off;
		break;
	case SEEK_END:
		newpos = dev->size + off;
		break;
	default:
		return -EINVAL;
	}
	if (newpos < 0)
		return -EINVAL;
	filp->f_pos = newpos;
	return newpos;
}

static const struct file_operations scull_fops = {
	.owner   = THIS_MODULE,
	.llseek  = scull_llseek,
	.read    = scull_read,
	.write   = scull_write,
	.open    = scull_open,
	.release = scull_release,
};

/* ================================================================
 * Device setup / teardown helpers
 * ================================================================ */

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int devno = scull_devno + index;

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	cdev_add(&dev->cdev, devno, 1);
}

/*
 * Initialise one scull_dev for a given family.
 *   family: 0 = scullp, 1 = scullv, 2 = scullm
 *   idx:    index within family (0..scull_nr_devs-1)
 */
static int scull_init_dev(struct scull_dev *dev, int family, int idx)
{
	memset(dev, 0, sizeof(*dev));
	mutex_init(&dev->lock);
	dev->qset = scull_p_qset;

	switch (family) {
	case 0: /* scullp — page-based */
		dev->order   = scullp_order;
		dev->quantum = PAGE_SIZE << scullp_order;
		dev->alloc_quantum = scullp_alloc_quantum;
		dev->free_quantum  = scullp_free_quantum;
		dev->alloc_qset    = scullp_alloc_qset;
		dev->free_qset     = scullp_free_qset;
		break;

	case 1: /* scullv — vmalloc-based */
		dev->order   = scullv_order;
		dev->quantum = PAGE_SIZE << scullv_order;
		dev->alloc_quantum = scullv_alloc_quantum;
		dev->free_quantum  = scullv_free_quantum;
		dev->alloc_qset    = scullv_alloc_qset;
		dev->free_qset     = scullv_free_qset;
		break;

	case 2: /* scullm — mempool for qset nodes */
		dev->order   = scullm_order;
		dev->quantum = PAGE_SIZE << scullm_order;

		dev->qset_cache = kmem_cache_create(
			"scullm_qset",
			sizeof(struct scull_qset), 0,
			SLAB_HWCACHE_ALIGN, NULL);
		if (!dev->qset_cache)
			return -ENOMEM;

		dev->qset_pool = mempool_create(
			scullm_pool_min_nr,
			mempool_alloc_slab, mempool_free_slab,
			dev->qset_cache);
		if (!dev->qset_pool) {
			kmem_cache_destroy(dev->qset_cache);
			dev->qset_cache = NULL;
			return -ENOMEM;
		}

		dev->alloc_quantum = scullm_alloc_quantum;
		dev->free_quantum  = scullm_free_quantum;
		dev->alloc_qset    = scullm_alloc_qset;
		dev->free_qset     = scullm_free_qset;
		break;
	}
	return 0;
}

/* ================================================================
 * Module init / exit
 * ================================================================ */

static void scull_cleanup_module(void)
{
	int i, family, idx;

	if (!scull_devices)
		goto unregister;

	for (i = 0; i < SCULL_TOTAL; i++) {
		struct scull_dev *dev = &scull_devices[i];

		family = i / scull_nr_devs;
		idx    = i % scull_nr_devs;

		/* remove sysfs device node */
		device_destroy(scull_class, scull_devno + i);

		cdev_del(&dev->cdev);

		/* free stored data */
		scull_trim(dev);

		/* destroy mempool resources if scullm */
		if (family == 2) {
			if (dev->qset_pool)
				mempool_destroy(dev->qset_pool);
			if (dev->qset_cache)
				kmem_cache_destroy(dev->qset_cache);
		}
	}
	kfree(scull_devices);

unregister:
	if (scull_class) {
		class_destroy(scull_class);
		scull_class = NULL;
	}
	if (scull_devno)
		unregister_chrdev_region(scull_devno, SCULL_TOTAL);
}

static int __init scull_init_module(void)
{
	int result, i, family, idx;
	dev_t devno;

	/* --- allocate device numbers --- */
	if (scull_major) {
		scull_devno = MKDEV(scull_major, scull_minor);
		result = register_chrdev_region(scull_devno, SCULL_TOTAL,
						"scull_exam");
	} else {
		result = alloc_chrdev_region(&scull_devno, scull_minor,
					     SCULL_TOTAL, "scull_exam");
		scull_major = MAJOR(scull_devno);
	}
	if (result < 0) {
		pr_warn("scull_exam: can't get major %d\n", scull_major);
		return result;
	}

	/* --- create sysfs class for udev --- */
	scull_class = class_create("scull_exam");
	if (IS_ERR(scull_class)) {
		result = PTR_ERR(scull_class);
		scull_class = NULL;
		unregister_chrdev_region(scull_devno, SCULL_TOTAL);
		return result;
	}

	/* --- allocate device array --- */
	scull_devices = kzalloc(SCULL_TOTAL * sizeof(struct scull_dev),
				GFP_KERNEL);
	if (!scull_devices) {
		result = -ENOMEM;
		goto fail;
	}

	/* --- initialise each device --- */
	for (i = 0; i < SCULL_TOTAL; i++) {
		family = i / scull_nr_devs;
		idx    = i % scull_nr_devs;

		result = scull_init_dev(&scull_devices[i], family, idx);
		if (result)
			goto fail;

		scull_setup_cdev(&scull_devices[i], i);

		devno = scull_devno + i;
		device_create(scull_class, NULL, devno, NULL,
			      "%s%d", family_name[family], idx);
	}

	pr_info("scull_exam: loaded (major %d, %d devices per family)\n",
		scull_major, scull_nr_devs);
	return 0;

fail:
	scull_cleanup_module();
	return result;
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);

MODULE_AUTHOR("Student");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Three-backend scull driver (pages/vmalloc/mempool) — LDD3-inspired exam module");
