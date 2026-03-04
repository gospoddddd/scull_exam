#include <linux/module.h> // Основной заголовок для всех модулей ядра, содержит макросы и функции для инициализации и очистки модуля, а также определения для работы с модулями.
#include <linux/moduleparam.h> // Заголовок для работы с параметрами модуля, позволяет объявлять параметры, которые могут быть установлены при загрузке модуля или через sysfs.
#include <linux/init.h> // Заголовок для макросов __init и __exit, которые используются для обозначения функций инициализации и очистки модуля.
#include <linux/kernel.h> // Заголовок для основных функций и макросов ядра, таких как printk, container_of и других.
#include <linux/slab.h> // 	Заголовок для функций выделения и освобождения памяти в ядре, таких как kmalloc, kfree, kzalloc и других.
#include <linux/fs.h> // Заголовок для работы с файловой системой, содержит определения для структур inode, file, file_operations и других, необходимых для реализации драйвера устройств.
#include <linux/errno.h> // Заголовок для кодов ошибок, таких как -ENOMEM, -EFAULT, -EINVAL и других, которые используются для обозначения различных ошибок в функциях драйвера.
#include <linux/types.h> // Заголовок для основных типов данных, таких как dev_t, loff_t и других, которые используются в драйвере устройств.
#include <linux/cdev.h> // Заголовок для работы с символьными устройствами, содержит определения для структуры cdev и функций для её инициализации и регистрации.
#include <linux/device.h> // Заголовок для работы с устройствами в sysfs, содержит определения для структуры class и функций для создания и уничтожения устройств.
#include <linux/vmalloc.h> // Заголовок для работы с виртуальной памятью, содержит определения для функций vmalloc, vfree и других, которые используются для выделения и освобождения виртуальной памяти в драйвере.
#include <linux/uaccess.h> // Заголовок для работы с пользовательским пространством, содержит определения для функций copy_to_user, copy_from_user и других, которые используются для безопасного обмена данными между ядром и пользовательским пространством.
#include <linux/mempool.h>// Заголовок для работы с mempool, содержит определения для структуры mempool_t и функций для создания, выделения и освобождения объектов из mempool, которые используются в одном из backend-ов драйвера.
#include <linux/string.h> // Заголовок для работы со строками и памятью, содержит определения для функций memset, memcpy и других, которые используются для инициализации и копирования данных в драйвере.

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

static void *scullp_alloc_quantum(struct scull_dev *dev) // Выделение кванта для scullp
{
	void *p = (void *)__get_free_pages(GFP_KERNEL, dev->order); // __get_free_pages — выделяет непрерывный блок страниц, возвращая указатель на первую страницу. Первый аргумент — флаги аллокации, второй аргумент — порядок блока (количество страниц = 2^order).
	if (p)
		memset(p, 0, PAGE_SIZE << dev->order);
	return p;
}

static void scullp_free_quantum(struct scull_dev *dev, void *ptr)
{
	free_pages((unsigned long)ptr, dev->order); // free_pages — освобождает блок страниц, выделенный __get_free_pages. Первый аргумент — указатель на первую страницу (приводится к unsigned long), второй аргумент — порядок блока (должен совпадать с порядком, использованным при выделении).
}

static struct scull_qset *scullp_alloc_qset(struct scull_dev *dev)
{
	return kzalloc(sizeof(struct scull_qset), GFP_KERNEL); // kzalloc — выделяет память, инициализируя её нулями. Первый аргумент — размер в байтах, второй аргумент — флаги аллокации.
}

static void scullp_free_qset(struct scull_dev *dev, struct scull_qset *qs)
{
	kfree(qs); 
}

/* ================================================================
 * vmalloc (scullv)
 * ================================================================ */

static void *scullv_alloc_quantum(struct scull_dev *dev) // Выделение кванта для scullv
{
	void *p = vmalloc(PAGE_SIZE << dev->order); // vmalloc — выделяет непрерывный блок виртуальной памяти, который может быть физически не непрерывным. Аргумент — размер в байтах.
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

	for (dptr = dev->data; dptr; dptr = next) { // Идет по linked list qset-узлов. 
		if (dptr->data) {
			for (i = 0; i < qset; i++)
				if (dptr->data[i])
					dev->free_quantum(dev, dptr->data[i]); // Освобождает квант
			kfree(dptr->data); // Освобождает массив указателей на кванты внутри qset-узла.
			dptr->data = NULL;
		}
		next = dptr->next;
		dev->free_qset(dev, dptr); // Освобождает сам qset-узел.
	}
	dev->size = 0;
	dev->data = NULL;
	return 0;
}

/*
 * scull_follow — пройти (и при необходимости создать) связный список qset
 * до элемента с номером <n>. Возвращает NULL при ошибке выделения памяти.
 */
static struct scull_qset *scull_follow(struct scull_dev *dev, int n) // функция проходит по linked list при необходимости создает qset-узлы
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

	dev = container_of(inode->i_cdev, struct scull_dev, cdev); // По адресу встроенного поля cdev восстанавливает адрес всей struct scull_dev.
	filp->private_data = dev; // Сохраняет указатель на структуру устройства в поле private_data структуры file, чтобы другие функции могли получить доступ к данным устройства.

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
		dptr->data = kcalloc(qset, sizeof(void *), GFP_KERNEL); // kcalloc — выделяет память для массива из qset элементов, каждый размером sizeof(void *), и инициализирует её нулями. Аргументы: количество элементов, размер каждого элемента, флаги аллокации.
		if (!dptr->data)
			goto out;
	}
	if (!dptr->data[s_pos]) {
		dptr->data[s_pos] = dev->alloc_quantum(dev); // Выделяет квант для данного узла и сохраняет указатель в массиве data внутри qset-узла. Если выделение не удалось, возвращает ошибку.
		if (!dptr->data[s_pos])
			goto out;
	}

	/* пишем только до конца текущего кванта */
	if (count > quantum - q_pos)
		count = quantum - q_pos;

	if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) { // copy_from_user — копирует данные из пользовательского пространства в ядро. Первый аргумент — указатель на буфер в ядре, второй аргумент — указатель на буфер в пользовательском пространстве, третий аргумент — количество байт для копирования. Возвращает количество байт, которые не удалось скопировать (0 при успехе), поэтому условие проверяет, если результат не равен 0, значит произошла ошибка.
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

static loff_t scull_llseek(struct file *filp, loff_t off, int whence) // Устанавливают новую позицию и возвращают ее.
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

static const struct file_operations scull_fops = { // Структура file_operations, которая связывает операции с функциями драйвера. 
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

	cdev_init(&dev->cdev, &scull_fops); // Инициализирует встроенный cdev и привязывает к нему scull_fops.
	dev->cdev.owner = THIS_MODULE;
	cdev_add(&dev->cdev, devno, 1); // Регистрирует char device в ядре.
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

MODULE_AUTHOR("gospoddddd");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Three-backend scull driver (pages/vmalloc/mempool) — Yuri Pavlovich, I really don't want to join the army :( ");
