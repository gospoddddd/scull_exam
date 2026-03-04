// SPDX-License-Identifier: GPL-2.0
/*
 * scull_exam — драйвер символьного устройства с тремя backend, вдохновлённый LDD3 scull/scullp/scullv/scullc.
 *
 * Семейства:
 *   scullp0..3  — кванты через __get_free_pages / free_pages
 *   scullv0..3  — кванты через vmalloc / vfree
 *   scullc0..3  — кванты из slab-кэша (kmem_cache_alloc/free)
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
#include <linux/string.h>

#include "scull_exam.h"

/* ================================================================
 * Параметры модуля
 * ================================================================ */

int scull_major;                   /* 0 => динамическое выделение        */
int scull_minor;                   /* первый minor-номер             */
int scull_nr_devs   = SCULL_NR_DEVS; /* устройств в семействе           */
int scull_p_qset    = 1000;        /* qset по умолчанию (указателей/узел)   */
int scullp_order    = 0;           /* порядок страниц для scullp          */
int scullv_order    = 4;           /* порядок страниц для scullv          */
int scullc_quantum  = 4000;        /* размер кванта для scullc (slab-кэш) */

module_param(scull_major, int, S_IRUGO);
module_param(scull_minor, int, S_IRUGO);
module_param(scull_nr_devs, int, S_IRUGO);
module_param(scull_p_qset, int, S_IRUGO);
module_param(scullp_order, int, S_IRUGO);
module_param(scullv_order, int, S_IRUGO);
module_param(scullc_quantum, int, S_IRUGO);

/* ================================================================
 * Глобальные переменные
 * ================================================================ */

static struct scull_dev *scull_devices;  /* массив из SCULL_TOTAL устройств */
static struct class     *scull_class;    /* класс sysfs для узлов udev   */
static dev_t             scull_devno;    /* первый dev_t (major+minor)    */
static struct kmem_cache *scullc_cache;  /* общий slab-кэш квантов scullc */

/* префиксы имён устройств, индексируются семейством */
static const char * const family_name[] = { "scullp", "scullv", "scullc" };

/* ================================================================
 * Хелперы backend — страницы (scullp)
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
 * Хелперы backend — vmalloc (scullv)
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
 * Хелперы backend — cache (scullc)
 * ================================================================ */

static void *scullc_alloc_quantum(struct scull_dev *dev)
{
	void *p = kmem_cache_alloc(scullc_cache, GFP_KERNEL);
	if (p)
		memset(p, 0, dev->quantum);
	return p;
}

static void scullc_free_quantum(struct scull_dev *dev, void *ptr)
{
	kmem_cache_free(scullc_cache, ptr);
}

static struct scull_qset *scullc_alloc_qset(struct scull_dev *dev)
{
	return kzalloc(sizeof(struct scull_qset), GFP_KERNEL);
}

static void scullc_free_qset(struct scull_dev *dev, struct scull_qset *qs)
{
	kfree(qs);
}

/* ================================================================
 * Общая часть — scull_trim / scull_follow
 * ================================================================ */

/*
 * scull_trim — освободить всё хранилище, занятое устройством.
 * Вызывающий код должен держать dev->lock.
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
 * scull_follow — пройти (и при необходимости создать) связный список qset
 * до элемента с номером <n>. Возвращает NULL при ошибке выделения памяти.
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
 * Операции файла
 * ================================================================ */

static int scull_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	/* если открыт только на запись — очищаем (классическое поведение scull) */
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
	int itemsize = quantum * qset;  /* байт на один узел связного списка */
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

	/* читаем только до конца текущего кванта */
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

	/* пишем только до конца текущего кванта */
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
 * Хелперы инициализации / освобождения устройств
 * ================================================================ */

static void scull_setup_cdev(struct scull_dev *dev, int index)
{
	int devno = scull_devno + index;

	cdev_init(&dev->cdev, &scull_fops);
	dev->cdev.owner = THIS_MODULE;
	cdev_add(&dev->cdev, devno, 1);
}

/*
 * Инициализировать один scull_dev для заданного семейства.
 *   family: 0 = scullp, 1 = scullv, 2 = scullc
 *   idx:    индекс внутри семейства (0..scull_nr_devs-1)
 */
static int scull_init_dev(struct scull_dev *dev, int family, int idx)
{
	memset(dev, 0, sizeof(*dev));
	mutex_init(&dev->lock);
	dev->qset = scull_p_qset;

	switch (family) {
	case 0: /* scullp — постраничный backend */
		dev->order   = scullp_order;
		dev->quantum = PAGE_SIZE << scullp_order;
		dev->alloc_quantum = scullp_alloc_quantum;
		dev->free_quantum  = scullp_free_quantum;
		dev->alloc_qset    = scullp_alloc_qset;
		dev->free_qset     = scullp_free_qset;
		break;

	case 1: /* scullv — backend на vmalloc */
		dev->order   = scullv_order;
		dev->quantum = PAGE_SIZE << scullv_order;
		dev->alloc_quantum = scullv_alloc_quantum;
		dev->free_quantum  = scullv_free_quantum;
		dev->alloc_qset    = scullv_alloc_qset;
		dev->free_qset     = scullv_free_qset;
		break;

	case 2: /* scullc — кванты из slab-кэша */
		dev->order   = 0;
		dev->quantum = scullc_quantum;
		dev->alloc_quantum = scullc_alloc_quantum;
		dev->free_quantum  = scullc_free_quantum;
		dev->alloc_qset    = scullc_alloc_qset;
		dev->free_qset     = scullc_free_qset;
		break;
	}
	return 0;
}

/* ================================================================
 * Инициализация / выгрузка модуля
 * ================================================================ */

static void scull_cleanup_module(void)
{
	int i;

	if (!scull_devices)
		goto unregister;

	for (i = 0; i < SCULL_TOTAL; i++) {
		struct scull_dev *dev = &scull_devices[i];

		/* удалить узел устройства из sysfs */
		device_destroy(scull_class, scull_devno + i);

		cdev_del(&dev->cdev);

		/* освободить сохранённые данные */
		scull_trim(dev);

		/* дополнительных ресурсов на уровне устройства нет */
	}
	kfree(scull_devices);

unregister:
	if (scull_class) {
		class_destroy(scull_class);
		scull_class = NULL;
	}
	if (scull_devno)
		unregister_chrdev_region(scull_devno, SCULL_TOTAL);
	if (scullc_cache) {
		kmem_cache_destroy(scullc_cache);
		scullc_cache = NULL;
	}
}

static int __init scull_init_module(void)
{
	int result, i, family, idx;
	dev_t devno;

	/* --- выделение номеров устройств --- */
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

	/* --- создать общий slab-кэш квантов для scullc --- */
	if (scullc_quantum <= 0) {
		result = -EINVAL;
		goto fail_unregister;
	}
	scullc_cache = kmem_cache_create("scullc", scullc_quantum, 0,
					 SLAB_HWCACHE_ALIGN, NULL);
	if (!scullc_cache) {
		result = -ENOMEM;
		goto fail_unregister;
	}

	/* --- создание класса sysfs для udev --- */
	scull_class = class_create("scull_exam");
	if (IS_ERR(scull_class)) {
		result = PTR_ERR(scull_class);
		scull_class = NULL;
		goto fail_unregister;
	}

	/* --- выделение массива устройств --- */
	scull_devices = kzalloc(SCULL_TOTAL * sizeof(struct scull_dev),
				GFP_KERNEL);
	if (!scull_devices) {
		result = -ENOMEM;
		goto fail;
	}

	/* --- инициализация каждого устройства --- */
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

fail_unregister:
	if (scullc_cache) {
		kmem_cache_destroy(scullc_cache);
		scullc_cache = NULL;
	}
	unregister_chrdev_region(scull_devno, SCULL_TOTAL);
	return result;
}

module_init(scull_init_module);
module_exit(scull_cleanup_module);

MODULE_AUTHOR("Student");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Three-backend scull driver (pages/vmalloc/scullc-cache) — LDD3-inspired exam module");
