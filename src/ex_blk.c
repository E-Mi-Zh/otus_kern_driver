#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/blkdev.h>

#define DEVICE_NAME "ex_blk"

struct ex_blk_dev {
    struct gendisk *disk;
    bool disk_added;
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

    pr_info("[INIT] module loaded\n");
    return 0;
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
