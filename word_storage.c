#define pr_fmt(fmt) "wordsDev: " fmt
#include <linux/string.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/gpio/driver.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/random.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tashana Mehta-Wilson");
MODULE_DESCRIPTION("A module to write to and read from a file and log a random word when a gpio pin goes high.");

#define MAX_STORAGE (64 * 1024)

struct storage {
    char    *buf;
    size_t   buf_len;
    char *words_split;
    char **words;
    int num_words;
    struct mutex lock;
};

static struct storage word_storage;

static char *gpio_chip_name = "sim_chip";
module_param(gpio_chip_name, charp, 0644);

static int gpio_offset = 0;
module_param(gpio_offset, int, 0644);

static int gpio_pin = -1;
static int gpio_irq;

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
    char *temp, *new_buf, *split, *cur, *word;
    char **words;
    size_t new_len;
    int i = 0;
    ssize_t ret;

    if (count == 0)
        return 0;

    temp = kmalloc(count, GFP_KERNEL);
    if (!temp)
        return -ENOMEM;

    if (copy_from_user(temp, buf, count)) {
        kfree(temp);
        return -EFAULT;
    }

    mutex_lock(&word_storage.lock);

    /* Get the new total length stored in the buffer but don't update word_storage.buf_len yet 
     * so that after an early return only the original contents of word_storage.buf will be read. */
    new_len = word_storage.buf_len + count;
    if (new_len > MAX_STORAGE) {
        ret = -EFBIG;
        goto out;
    }

    /* Increase the size of the storage buffer, keeping the old contents. */
    new_buf = krealloc(word_storage.buf, new_len + 1, GFP_KERNEL);
    if (!new_buf) {
        ret = -ENOMEM;
        goto out;
    }
    word_storage.buf = new_buf;

    memcpy(new_buf + word_storage.buf_len, temp, count);
    new_buf[new_len] = '\0';

    /* re-split the whole text */
    split = kstrdup(new_buf, GFP_KERNEL);
    if (!split) {
        ret = -ENOMEM;
        goto out;
    }

    words = kmalloc_array(new_len / 2 + 1, sizeof(*words), GFP_KERNEL);
    if (!words) {
        kfree(split);
        ret = -ENOMEM;
        goto out;
    }

    cur = split;
    while ((word = strsep(&cur, " \t\r\n")) != NULL) {
        if (*word == '\0')
            continue;
        words[i++] = word;
    }

    /* Free old storage and replace values */
    kfree(word_storage.words_split);
    kfree(word_storage.words);
    word_storage.buf_len     = new_len;
    word_storage.words_split = split;
    word_storage.words       = words;
    word_storage.num_words   = i;

    pr_info("stored %zu bytes (%zu total, %d words)\n", count, new_len, i);
    ret = count;

out:
    mutex_unlock(&word_storage.lock);
    kfree(temp);
    return ret;
}

static void storage_clear(void)
{
    kfree(word_storage.buf);
    kfree(word_storage.words_split);
    kfree(word_storage.words);
    word_storage.buf = NULL;
    word_storage.buf_len = 0;
    word_storage.words_split = NULL;
    word_storage.words = NULL;
    word_storage.num_words = 0;
}

static int words_open(struct inode *inode, struct file *file)
{
    if ((file->f_mode & FMODE_WRITE) && (file->f_flags & O_TRUNC)) {
        mutex_lock(&word_storage.lock);
        storage_clear();
        mutex_unlock(&word_storage.lock);
    }
    return 0;
}

static const struct file_operations devFileOps = {
    .owner = THIS_MODULE,
    .read = words_read,
    .write = words_write,
    .open = words_open,
};

static struct miscdevice miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "wordsDev",
    .fops = &devFileOps,
};

static int match_chip_by_name(struct gpio_chip *gc, const void *data)
{
    const char *name = data;
    if (!gc || !gc->label || !name) {
        return 0;
    }
    return sysfs_streq(gc->label, name);
}

static irqreturn_t gpio_irq_thread_handler(int irq, void *data)
{
    mutex_lock (&word_storage.lock);
    if (word_storage.num_words <= 0) {
        pr_info ("No words to log\n");
        goto end;
    }

    pr_info ("Randomly selected word: %s\n", word_storage.words[get_random_u32_below(word_storage.num_words)]);

    end:
    mutex_unlock(&word_storage.lock);
    return IRQ_HANDLED;
}

static int gpio_init (void)
{
    struct gpio_chip *chip;
    int ret;

    struct gpio_device *gpio_dev = gpio_device_find(gpio_chip_name, match_chip_by_name);
    if (!gpio_dev) {
        pr_err("Could not find GPIO device '%s'.\n", gpio_chip_name);
        return -ENODEV;
    }

    chip = gpio_device_get_chip(gpio_dev);
    if (!chip)
    {
        gpio_device_put (gpio_dev);
        return -ENODEV;
    }

    if (gpio_offset < 0 || gpio_offset >= chip->ngpio) {
        pr_err("Invalid GPIO line offset %d.\n", gpio_offset);
        gpio_device_put (gpio_dev);
        return -EINVAL;
    }

    gpio_pin = chip->base + gpio_offset;
    gpio_device_put (gpio_dev);

    if(!gpio_is_valid(gpio_pin)) {
        pr_err("Invalid GPIO pin.\n");
        return -EINVAL;
    }

    ret = gpio_request (gpio_pin, "WordIRQ");
    if (ret)
        return ret;

    ret = gpio_direction_input (gpio_pin);
    if (ret)
        goto out;

    gpio_irq = gpio_to_irq(gpio_pin);
    if (gpio_irq < 0) {
        ret = gpio_irq;
        goto out;
    }

    ret = request_threaded_irq (gpio_irq, NULL, gpio_irq_thread_handler, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "WordIRQ", NULL);
    if (ret)
        goto out;

    return 0;

    out:
    gpio_free (gpio_pin);
    return ret;

}

static int __init words_init (void)
{

    int ret;

    word_storage.buf = NULL;
    word_storage.buf_len = 0;
    word_storage.words_split = NULL;
    word_storage.words = NULL;
    word_storage.num_words = 0;
    mutex_init(&word_storage.lock);

    ret = misc_register(&miscdev);

    if (ret) {
        pr_err("Failed to load module\n");
        return ret;
    }

    ret = gpio_init();
    if(ret) {
        misc_deregister(&miscdev);
        return ret;
    }

    pr_info("Module loaded\n");

    return 0;
}

static void __exit words_exit (void)
{
    free_irq(gpio_irq, NULL);
    gpio_free(gpio_pin);

    misc_deregister (&miscdev);

    /* No lock is needed after the irq is freed and miscDev is deregistered as nothing else will touch word_storage. */
    storage_clear();

    pr_info("Module unloaded\n");
}

module_init (words_init);
module_exit (words_exit);

