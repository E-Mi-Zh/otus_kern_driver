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
    struct blk_mq_tag_set* tag_set;
    u8 *data;
};

static struct ex_blk_dev *blk_dev = NULL;
static int dev_major = 0;

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

    blk_dev->disk->major = dev_major;
    blk_dev->disk->first_minor = 0;
    blk_dev->disk->minors = 1; // no partitions yet
    strscpy(blk_dev->disk->disk_name, DEVICE_NAME, sizeof(blk_dev->disk->disk_name));
    set_capacity(blk_dev->disk, blk_dev->capacity);

    if (add_disk(blk_dev->disk))
        goto err;
    blk_dev->disk_added = true;

    pr_info("[INIT] module loaded\n");
    return 0;

err:
    if (blk_dev) {
        if (blk_dev->data) vfree(blk_dev->data);
        kfree(blk_dev->tag_set);
        if (blk_dev->disk) put_disk(blk_dev->disk);
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
