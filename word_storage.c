#define pr_fmt(fmt) "wordsDev: " fmt
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tashana Mehta-Wilson");
MODULE_DESCRIPTION("A module to write to and read from a file");


static int __init words_init (void) {
    pr_info("Module loaded\n");

    return 0;
}


static void __exit words_exit (void) {
    pr_info("Module unloaded\n");
}


module_init (words_init);
module_exit (words_exit);

