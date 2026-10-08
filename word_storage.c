#define pr_fmt(fmt) "wordsDev: " fmt
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tashana Mehta-Wilson");
MODULE_DESCRIPTION("A module to write to and read from a file");

struct storage {
    char    *buf;
    size_t   buf_len;
    struct mutex lock;
};

static struct storage word_storage;

static ssize_t words_read (struct file *filp, char __user *buf,
                           size_t count, loff_t *ppos)
{
    ssize_t ret;

    mutex_lock(&word_storage.lock);

    if (*ppos >= word_storage.buf_len) {
        ret = 0;  /* EOF */
        goto out;
    }

    count = min(count, word_storage.buf_len - (size_t)*ppos);
    if (copy_to_user(buf, word_storage.buf + *ppos, count)) {
        ret = -EFAULT;
        goto out;
    }

    *ppos += count;
    ret = count;

out:
    mutex_unlock(&word_storage.lock);
    return ret;

}

static ssize_t words_write(struct file *file, const char __user *buf,
                                size_t count, loff_t *ppos)
{
    char *temp;

    temp = kmalloc(count + 1, GFP_KERNEL);
    if (!temp)
        return -ENOMEM;

    if (copy_from_user(temp, buf, count)) {
        kfree(temp);
        return -EFAULT;
    }
    temp[count] = '\0';

    mutex_lock(&word_storage.lock);
    kfree(word_storage.buf);          // free whatever was stored before (replace semantics)
    word_storage.buf = temp;
    word_storage.buf_len = count;
    mutex_unlock(&word_storage.lock);

    printk(KERN_INFO "wordstore: stored %zu bytes\n", count);

    return count;

}

static const struct file_operations devFileOps = {
    .owner = THIS_MODULE,
    .read = words_read,
    .write = words_write,
    
};

static struct miscdevice miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "wordsDev",
    .fops = &devFileOps,
};

static int __init words_init (void) 
{
    int ret;

    word_storage.buf = NULL;
    word_storage.buf_len = 0;
    mutex_init(&word_storage.lock);

    ret = misc_register(&miscdev);

    if (ret) {
        pr_err("Failed to load module\n");
        return ret;
    }
    pr_info("Module loaded\n");

    return 0;
}

static void __exit words_exit (void) 
{
    mutex_lock(&word_storage.lock);
    kfree(word_storage.buf);
    word_storage.buf = NULL;
    mutex_unlock(&word_storage.lock);
    
    misc_deregister (&miscdev);

    pr_info("Module unloaded\n");
}


module_init (words_init);
module_exit (words_exit);

