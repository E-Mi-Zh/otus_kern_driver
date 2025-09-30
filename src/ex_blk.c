#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/blk-mq.h>
#include <linux/kdev_t.h>

#define DEVICE_NAME "ex_blk"

#define NUM_PARTS 3
#define PART_SIZE_MB 100
#define PART_SIZE_BYTES (PART_SIZE_MB * 1024 * 1024ULL)
#define PART_SECTORS (PART_SIZE_BYTES / SECTOR_SIZE)
#define TOTAL_SECTORS (NUM_PARTS * PART_SECTORS)
#define TOTAL_BYTES (NUM_PARTS * PART_SIZE_BYTES)

struct ex_blk_dev {
	struct gendisk *disk;
	bool disk_added;
	sector_t capacity;
	struct blk_mq_tag_set *tag_set;
	u8 *data;
};

static struct ex_blk_dev *blk_dev = NULL;
static int dev_major = 0;

static struct blk_mq_ops ex_blk_mq_ops;
static struct block_device_operations ex_blk_fops;

static int ex_blk_handle_request(struct request *rq)
{
	struct ex_blk_dev *dev = rq->q->queuedata;
	sector_t pos = blk_rq_pos(rq);
	sector_t dev_sectors = dev->capacity;
	struct bio_vec bvec;
	struct req_iterator iter;
	int dir = rq_data_dir(rq);
	sector_t sector_count;
	size_t len;
	void *buf;

	if (pos >= dev_sectors)
		return -EIO;

	rq_for_each_segment(bvec, rq, iter) {
		sector_count = bvec.bv_len >> SECTOR_SHIFT;
		if (pos + sector_count > dev_sectors)
			sector_count = dev_sectors - pos;
		if (sector_count == 0)
			break;

		len = sector_count << SECTOR_SHIFT;
		buf = page_address(bvec.bv_page) + bvec.bv_offset;
		if (dir == WRITE)
			memcpy(dev->data + (pos << SECTOR_SHIFT), buf, len);
		else
			memcpy(buf, dev->data + (pos << SECTOR_SHIFT), len);
		pos = pos + sector_count;
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
	return BLK_STS_OK;
}

static int ex_blk_open(struct block_device *blkdev, fmode_t mode)
{
	return 0;
}

static void ex_blk_release(struct gendisk *disk, fmode_t mode)
{
}

static int ex_blk_ioctl(struct block_device *blkdev, fmode_t mode,
			unsigned int cmd, unsigned long arg)
{
	return 0;
}

static int __init ex_blk_init(void)
{
	dev_major = register_blkdev(0, DEVICE_NAME);
	if (dev_major < 0) {
		pr_err("[INIT] register_blkdev failed\n");
		return dev_major;
	}

	blk_dev = kzalloc(sizeof(*blk_dev), GFP_KERNEL);
	if (!blk_dev)
		goto err;

	blk_dev->capacity = TOTAL_SECTORS;
	blk_dev->data = vmalloc(TOTAL_BYTES);
	if (!blk_dev->data)
		goto err;

	blk_dev->disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!blk_dev->disk)
		goto err;

	blk_dev->tag_set = kzalloc(sizeof(*blk_dev->tag_set), GFP_KERNEL);
	if (!blk_dev->tag_set)
		goto err;

	ex_blk_mq_ops.queue_rq = ex_blk_queue_rq;
	blk_dev->tag_set->ops = &ex_blk_mq_ops;

	blk_dev->tag_set->nr_hw_queues = 1;
	blk_dev->tag_set->queue_depth = 128;
	blk_dev->tag_set->numa_node = NUMA_NO_NODE;
	blk_dev->tag_set->flags = BLK_MQ_F_SHOULD_MERGE;

	if (blk_mq_alloc_tag_set(blk_dev->tag_set))
		goto err;

	if (blk_mq_init_allocated_queue(blk_dev->tag_set, blk_dev->disk->queue))
		goto err;

	blk_dev->disk->queue->queuedata = blk_dev;
	blk_queue_logical_block_size(blk_dev->disk->queue, SECTOR_SIZE);

	ex_blk_fops.owner = THIS_MODULE;
	ex_blk_fops.open = ex_blk_open;
	ex_blk_fops.release = ex_blk_release;
	ex_blk_fops.ioctl = ex_blk_ioctl;
	blk_dev->disk->fops = &ex_blk_fops;

	blk_dev->disk->major = dev_major;
	blk_dev->disk->first_minor = 0;
	blk_dev->disk->minors = 1; // no partitions yet
	strscpy(blk_dev->disk->disk_name, DEVICE_NAME,
		sizeof(blk_dev->disk->disk_name));
	set_capacity(blk_dev->disk, blk_dev->capacity);

	if (add_disk(blk_dev->disk))
		goto err;
	blk_dev->disk_added = true;

	pr_info("[INIT] module loaded\n");
	return 0;

err:
	if (blk_dev) {
		if (blk_dev->data)
			vfree(blk_dev->data);
		kfree(blk_dev->tag_set);
		if (blk_dev->disk)
			put_disk(blk_dev->disk);
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
		if (blk_dev->tag_set) {
			blk_mq_free_tag_set(blk_dev->tag_set);
			kfree(blk_dev->tag_set);
			blk_dev->tag_set = NULL;
		}
		if (blk_dev->data) {
			vfree(blk_dev->data);
			blk_dev->data = NULL;
		}
		if (blk_dev->disk) {
			put_disk(blk_dev->disk);
			blk_dev->disk = NULL;
		}
		kfree(blk_dev);
		blk_dev = NULL;
	}

	if (dev_major > 0) {
		unregister_blkdev(dev_major, DEVICE_NAME);
		dev_major = 0;
	}

	pr_info("[EXIT] module unloaded\n");
}

module_init(ex_blk_init);
module_exit(ex_blk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jack");
MODULE_DESCRIPTION("Example of block device");
