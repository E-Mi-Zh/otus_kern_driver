#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>

static int __init ex_blk_init(void)
{
    pr_info("[INIT] module loaded\n");
    return 0;
}

static void __exit ex_blk_exit(void)
{
    pr_info("[EXIT] module unloaded\n");
}

module_init(ex_blk_init);
module_exit(ex_blk_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jack");
MODULE_DESCRIPTION("Example of block device");
