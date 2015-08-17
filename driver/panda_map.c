/* Device for mapping PandA register space into user space memory.
 *
 * A register space of 65536 double word registers (256KB) is allocated for
 * PandA configuration and core functionality.  This is mapped via the device
 * node /dev/panda.map defined here. */

#include <linux/module.h>

MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd");
MODULE_DESCRIPTION("PandA register interface");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("panda");
MODULE_VERSION("0");


static int __exit panda_map_init(void)
{
    printk(KERN_INFO "Loading PandA map\n");
    return 0;
}


static void __exit panda_map_exit(void)
{
    printk(KERN_INFO "Unloading PandA map\n");
}

module_init(panda_map_init);
module_exit(panda_map_exit);
