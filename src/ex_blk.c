#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/blkpg.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/blk-mq.h>
#include <linux/kdev_t.h>
#include <linux/proc_fs.h>
#include <linux/string.h>

#define DEVICE_NAME "ex_blk"
#define PROC_DIR_NAME DEVICE_NAME
#define PROC_FILE_NAME "capacity"
#define MAX_CAP_STRLEN 32

#define DRIVER_NAME DEVICE_NAME
#define SYSFS_ATTR_NAME "capacity"

static struct device *example_dev = NULL;
static struct device_attribute dev_attr_data;
static struct class *example_class;

#define NUM_PARTS 3
#define PART_SIZE_MB 100
#define PART_SIZE_BYTES (PART_SIZE_MB * 1024 * 1024ULL)
#define PART_SECTORS (PART_SIZE_BYTES / SECTOR_SIZE)
#define TOTAL_SECTORS \
	(NUM_PARTS * PART_SECTORS + 1) /* 1 extra sector for MBR */
#define TOTAL_BYTES (TOTAL_SECTORS * SECTOR_SIZE)

struct ex_blk_dev {
	struct gendisk *disk;
	bool disk_added;
	sector_t capacity;
	struct blk_mq_tag_set *tag_set;
	u8 *data;
};

static struct ex_blk_dev *blk_dev = NULL;
static int dev_major = 0;

struct mbr_partition {
	u8 boot_flag; /* 0x80 - active, 0x00 - inactive */
	u8 start_head;
	u8 start_sector; /* 0-5 bits - first sect, 6-7 - cyl */
	u8 start_cylinder;
	u8 part_type;
	u8 end_head;
	u8 end_sector;
	u8 end_cylinder;
	__le32 start_sector_lba;
	__le32 nr_sectors;
} __attribute__((packed));

static struct blk_mq_ops ex_blk_mq_ops;
static struct block_device_operations ex_blk_fops;

/* for /proc support */
static struct proc_dir_entry *proc_dir = NULL;
static struct proc_dir_entry *proc_file = NULL;
static struct proc_ops proc_fops;

static int ex_blk_handle_request(struct request *rq)
{
	struct ex_blk_dev *dev = rq->q->queuedata;
	sector_t pos = blk_rq_pos(rq);
	sector_t current_pos = pos;
	sector_t dev_sectors = dev->capacity;
	struct bio_vec bvec;
	struct req_iterator iter;
	int dir = rq_data_dir(rq);
	sector_t sector_count, remaining_sectors;
	size_t len;
	void *buf;

	if (pos >= dev_sectors) {
		pr_err(DEVICE_NAME
		       ": Request beyond device limits: pos sector: %lld, dev  sectors size: %lld\n",
		       pos, dev_sectors);
		return -EIO;
	}

	rq_for_each_segment(bvec, rq, iter) {
		sector_count = bvec.bv_len >> SECTOR_SHIFT;
		remaining_sectors = dev_sectors - current_pos;
		if (sector_count > remaining_sectors) {
			sector_count = remaining_sectors;
			if (sector_count <= 0)
				break;
		}
		if (sector_count == 0)
			break;

		len = sector_count << SECTOR_SHIFT;
		buf = page_address(bvec.bv_page) + bvec.bv_offset;
		if (!buf) {
			pr_err(DEVICE_NAME ": Failed to get buffer address\n");
			return -EIO;
		}

		if (dir == WRITE)
			memcpy(dev->data + (current_pos << SECTOR_SHIFT), buf,
			       len);
		else
			memcpy(buf, dev->data + (current_pos << SECTOR_SHIFT),
			       len);
		current_pos = current_pos + sector_count;
	}
	return 0;
}

static blk_status_t ex_blk_queue_rq(struct blk_mq_hw_ctx *hctx,
				    const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	blk_status_t status;

	blk_mq_start_request(rq);

	if (ex_blk_handle_request(rq) == 0)
		status = BLK_STS_OK;
	else
		status = BLK_STS_IOERR;

	blk_mq_end_request(rq, status);
	return status;
}

static int ex_blk_open(struct block_device *bdev, fmode_t mode)
{
	struct request_queue *q = bdev_get_queue(bdev);

	if (!q)
		return -ENXIO;

	if (!blk_get_queue(q))
		return -ENXIO;

	pr_info(DEVICE_NAME ": Device opened, minor=%d\n", MINOR(bdev->bd_dev));
	return 0;
}

static void ex_blk_release(struct gendisk *disk, fmode_t mode)
{
	struct request_queue *q = disk->queue;

	if (q)
		blk_put_queue(q);

	pr_info(DEVICE_NAME ": Device released\n");
}

static int ex_blk_ioctl(struct block_device *dev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	pr_info("BLKGETSIZE=0x%08x, BLKGETSIZE64=0x%08lx, HDIO_GETGEO=0x%08x, cmd=0x%08x\n",
		BLKGETSIZE, BLKGETSIZE64, HDIO_GETGEO, cmd);
	switch (cmd) {
	case BLKGETSIZE: {
		pr_info("BLKGETSIZE\n");
		unsigned long size = TOTAL_SECTORS;
		if (copy_to_user((void __user *)arg, &size, sizeof(size)))
			return -EFAULT;
		return 0;
	}
	case BLKGETSIZE64: {
		pr_info("BLKGETSIZE64\n");
		u64 size = TOTAL_BYTES;
		if (copy_to_user((void __user *)arg, &size, sizeof(size)))
			return -EFAULT;
		return 0;
	}
	case HDIO_GETGEO: {
		pr_info("HDIO_GETGEO\n");
		struct hd_geometry geo;
		memset(&geo, 0, sizeof(geo));
		geo.start = get_start_sect(dev);
		/* calc disk geometry */
		geo.heads = 16;
		geo.sectors = 63;
		geo.cylinders = TOTAL_SECTORS / (geo.heads * geo.sectors);

		if (copy_to_user((void __user *)arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	}
	case BLKRRPART:
		/* reread part table */
		return 0;
	default:
		pr_info(DEVICE_NAME ": Unknown ioctl: 0x%08x\n", cmd);
		break;
	}
	pr_info("ioctl %u 0x%08x\n", cmd, cmd);
	return -ENOTTY;
}

static void lba_to_chs(u32 lba, u8 *head, u8 *sector, u8 *cylinder)
{
	u32 temp;

	/* CHS: 16 heads, 63 sector per line */
	*sector = (lba % 63) + 1;
	temp = lba / 63;
	*head = temp % 16;
	*cylinder = (temp / 16) & 0xFF;

	/* higher adresses use max values */
	if (*cylinder > 1023) {
		*cylinder = 1023 & 0xFF;
		*head = 15;
		*sector = 63;
	}
}

static int init_mbr(struct ex_blk_dev *dev)
{
	struct mbr_partition *part;
	u8 *mbr = dev->data;
	int i;
	u32 start_sector, end_sector;
	u8 head, sector, cylinder;

	pr_info("Initializing MBR with %d partitions\n", NUM_PARTS);

	memset(mbr, 0, SECTOR_SIZE);
	start_sector =
		1; /* MBR in first sector (0), next sector first partition begin */

	/* 0x01BE part table offset */
	for (i = 0; i < NUM_PARTS; i++) {
		part = (struct mbr_partition
				*)(mbr + 0x1BE +
				   i * sizeof(struct mbr_partition));

		part->boot_flag = 0x00;

		/* CHS partition begins */
		lba_to_chs(start_sector, &head, &sector, &cylinder);
		part->start_head = head;
		part->start_sector = sector | ((cylinder >> 2) & 0xC0);
		part->start_cylinder = cylinder & 0xFF;

		/* Part type Linux (0x83) */
		part->part_type = 0x83;

		/* CHS part end */
		end_sector = start_sector + PART_SECTORS - 1;
		lba_to_chs(end_sector, &head, &sector, &cylinder);
		part->end_head = head;
		part->end_sector = sector | ((cylinder >> 2) & 0xC0);
		part->end_cylinder = cylinder & 0xFF;

		/* LBA offset and size */
		part->start_sector_lba = cpu_to_le32(start_sector);
		part->nr_sectors = cpu_to_le32(PART_SECTORS);

		pr_info("Partition %d: start_sector=%u, nr_sectors=%llu\n",
			i + 1, start_sector, PART_SECTORS);

		start_sector = start_sector + PART_SECTORS;
	}

	/* MBR sig */
	mbr[510] = 0x55;
	mbr[511] = 0xAA;

	pr_info("MBR initialized successfully\n");
	return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int proc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t proc_read(struct file *file, char __user *buf, size_t count,
			 loff_t *ppos)
{
	ssize_t ret = 0;
	size_t len;
	char data_buffer[MAX_CAP_STRLEN];

	len = snprintf(data_buffer, sizeof(data_buffer),
		       "Capacity: %llu sectors\n", blk_dev->capacity);

	if (len >= MAX_CAP_STRLEN - 1) {
		len = MAX_CAP_STRLEN - 1;
	}
	data_buffer[len] = '\0';

	// pr_info("data_buffer=%s len=%lu count=%lu\n", data_buffer, len, count);

	if (*ppos >= len) {
		return 0;
	}

	if (*ppos + count > len) {
		count = len - *ppos;
	}

	if (copy_to_user(buf, data_buffer + *ppos, count)) {
		return -EFAULT;
	}

	*ppos = *ppos + count;
	ret = count;

	return ret;
}

static ssize_t proc_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	char new_buffer[MAX_CAP_STRLEN];

	if (count > sizeof(new_buffer)) {
		count = sizeof(new_buffer);
	}

	if (copy_from_user(&new_buffer, buf, count)) {
		return -EFAULT;
	}

	new_buffer[count] = '\0';
	pr_info("Written to proc file: %s\n", new_buffer);

	return count;
}

static ssize_t data_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	ssize_t count = 0;
	char data_buffer[MAX_CAP_STRLEN];
	size_t len;

	len = snprintf(data_buffer, sizeof(data_buffer),
		       "Capacity: %llu sectors\n\n", blk_dev->capacity);

	if (len >= MAX_CAP_STRLEN - 1) {
		len = MAX_CAP_STRLEN - 1;
	}
	data_buffer[len] = '\0';

	count = scnprintf(buf, len, "%s", data_buffer);

	return count;
}

static ssize_t data_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	char new_buffer[MAX_CAP_STRLEN];

	if (count > 0) {
		memcpy(new_buffer, buf, count - 1);
		new_buffer[count] = '\0';
		pr_info("Written to sysfs file: %s\n", new_buffer);
	}

	return count;
}

static int __init ex_blk_init(void)
{
	int ret;

	dev_major = register_blkdev(0, DEVICE_NAME);
	if (dev_major < 0) {
		pr_err("[INIT] register_blkdev failed\n");
		return dev_major;
	}

	dev_attr_data.attr.name = SYSFS_ATTR_NAME;
	dev_attr_data.attr.mode = S_IWUSR | S_IRUSR | S_IRGRP | S_IWGRP |
				  S_IROTH | S_IWOTH;
	dev_attr_data.show = data_show;
	dev_attr_data.store = data_store;

	example_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(example_class)) {
		ret = PTR_ERR(example_class);
		pr_err("[INIT] Failed to create class: %d\n", ret);
		return ret;
	}
	example_dev = device_create(example_class, NULL, 0, NULL, DRIVER_NAME);
	if (IS_ERR(example_dev)) {
		ret = PTR_ERR(example_dev);
		pr_err("[INIT] Failed to create device: %d\n", ret);
		class_destroy(example_class);
		return ret;
	}

	ret = device_create_file(example_dev, &dev_attr_data);
	if (ret) {
		pr_err("Failed to create device file: %d\n", ret);
		device_destroy(example_class, 0);
		class_destroy(example_class);
		return ret;
	}
	pr_info("[INIT] Sysfs device file created: /sys/class/%s/%s/data\n",
		DRIVER_NAME, DRIVER_NAME);

	blk_dev = kzalloc(sizeof(*blk_dev), GFP_KERNEL);
	if (!blk_dev) {
		pr_err("[INIT] " DEVICE_NAME
		       ": Failed to allocate struct block_dev\n");
		goto err;
	}

	blk_dev->capacity = TOTAL_SECTORS;
	blk_dev->data = vmalloc(TOTAL_BYTES);
	if (!blk_dev->data) {
		pr_err("[INIT] " DEVICE_NAME
		       ": Failed to allocate device IO buffer\n");
		goto err;
	}

	init_mbr(blk_dev);

	blk_dev->disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!blk_dev->disk) {
		pr_err("[INIT] " DEVICE_NAME
		       ": Failed to allocate disk structure\n");
		goto err;
	}

	blk_dev->tag_set = kzalloc(sizeof(*blk_dev->tag_set), GFP_KERNEL);
	if (!blk_dev->tag_set) {
		pr_err("[INIT] " DEVICE_NAME
		       ": Failed to allocate memory for tag set struct!\n");
		goto err;
	}

	ex_blk_mq_ops.queue_rq = ex_blk_queue_rq;
	blk_dev->tag_set->ops = &ex_blk_mq_ops;

	blk_dev->tag_set->nr_hw_queues = 1;
	blk_dev->tag_set->nr_maps = 1;
	blk_dev->tag_set->queue_depth = 128;
	blk_dev->tag_set->numa_node = NUMA_NO_NODE;
	blk_dev->tag_set->flags = BLK_MQ_F_SHOULD_MERGE;

	if (blk_mq_alloc_tag_set(blk_dev->tag_set)) {
		pr_err("[INIT] " DEVICE_NAME ": Failed to allocate tag set\n");
		goto err;
	}

	if (blk_mq_init_allocated_queue(blk_dev->tag_set,
					blk_dev->disk->queue)) {
		pr_err("[INIT] " DEVICE_NAME ": Failed to init queue\n");
		goto err;
	}

	blk_dev->disk->queue->queuedata = blk_dev;
	blk_queue_logical_block_size(blk_dev->disk->queue, SECTOR_SIZE);

	ex_blk_fops.owner = THIS_MODULE;
	ex_blk_fops.open = ex_blk_open;
	ex_blk_fops.release = ex_blk_release;
	ex_blk_fops.ioctl = ex_blk_ioctl;
	blk_dev->disk->fops = &ex_blk_fops;

	blk_dev->disk->major = dev_major;
	blk_dev->disk->first_minor = 0;
	blk_dev->disk->minors = NUM_PARTS + 1;
	blk_dev->disk->private_data = &blk_dev;
	strscpy(blk_dev->disk->disk_name, DEVICE_NAME,
		sizeof(blk_dev->disk->disk_name));
	set_capacity(blk_dev->disk, blk_dev->capacity);

	if (add_disk(blk_dev->disk)) {
		pr_err("[INIT] " DEVICE_NAME ": Failed to add disk!\n");
		goto err;
	}
	blk_dev->disk_added = true;

	pr_info("[INIT] Device capacity: %llu sectors, buffer size: %llu bytes\n",
		blk_dev->capacity, TOTAL_BYTES);

	proc_fops.proc_open = proc_open;
	proc_fops.proc_read = proc_read;
	proc_fops.proc_write = proc_write;
	proc_fops.proc_release = proc_release;
	proc_dir = proc_mkdir(PROC_DIR_NAME, NULL);
	if (!proc_dir) {
		pr_err("[INIT] Failed to create proc directory\n");
		goto err;
	}
	proc_file = proc_create(PROC_FILE_NAME, 0666, proc_dir, &proc_fops);
	if (!proc_file) {
		pr_err("[INIT] Failed to create proc file\n");
		remove_proc_entry(PROC_DIR_NAME, NULL);
		goto err;
	}

	pr_info("[INIT] Proc file created: /proc/%s/%s\n", PROC_DIR_NAME,
		PROC_FILE_NAME);

	pr_info("[INIT] module loaded\n");
	return 0;

err:
	if (blk_dev) {
		if (blk_dev->data) {
			vfree(blk_dev->data);
		}
		if (blk_dev->tag_set) {
			if (blk_dev->tag_set->tags)
				blk_mq_free_tag_set(blk_dev->tag_set);
			kfree(blk_dev->tag_set);
		}
		if (blk_dev->disk) {
			put_disk(blk_dev->disk);
		}
		kfree(blk_dev);
	}
	if (dev_major > 0) {
		unregister_blkdev(dev_major, DEVICE_NAME);
		dev_major = 0;
	}
	return -ENOMEM;
}

static void __exit ex_blk_exit(void)
{
	if (blk_dev) {
		if (blk_dev->disk_added) {
			del_gendisk(blk_dev->disk);
			blk_dev->disk_added = false;
		}
		if (blk_dev->disk) {
			put_disk(blk_dev->disk);
			blk_dev->disk = NULL;
		}
		if (blk_dev->tag_set) {
			blk_mq_free_tag_set(blk_dev->tag_set);
			kfree(blk_dev->tag_set);
			blk_dev->tag_set = NULL;
		}
		if (blk_dev->data) {
			vfree(blk_dev->data);
			blk_dev->data = NULL;
		}
		kfree(blk_dev);
		blk_dev = NULL;
	}

	if (dev_major > 0) {
		unregister_blkdev(dev_major, DEVICE_NAME);
		dev_major = 0;
	}

	if (proc_file) {
		remove_proc_entry(PROC_FILE_NAME, proc_dir);
	}

	if (proc_dir) {
		remove_proc_entry(PROC_DIR_NAME, NULL);
	}

	if (example_dev) {
		device_remove_file(example_dev, &dev_attr_data);
		device_destroy(example_class, 0);
	}
	if (example_class) {
		class_destroy(example_class);
	}

	pr_info("[EXIT] module unloaded\n");
}

module_init(ex_blk_init);
module_exit(ex_blk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jack");
MODULE_DESCRIPTION("Example of block device");
