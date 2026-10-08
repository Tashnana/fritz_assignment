# word_storage – a Linux kernel module for storing words and logging them on GPIO events

An out-of-tree, dynamically loadable Linux kernel module that:

1. Logs a message to the kernel log when it is loaded and unloaded.
2. Provides a character device, `/dev/wordsDev`, that accepts text from userspace
   and stores it in dynamically allocated kernel memory.
3. Returns the stored text when the device is read.
4. Monitors a GPIO line (chosen with module parameters) through an interrupt, and
   logs a randomly selected stored word every time the line goes high.

The GPIO side was developed and tested with the `gpio-sim` simulated GPIO chip.

## Contents

| File | Description |
|------|-------------|
| `word_storage.c` | Module source |
| `Makefile` | Kbuild makefile for building against the running kernel |
| `example.log` | Kernel log from an example session |

## Environment

- Requires kernel 6.8 or newer (uses `gpio_device_find()` /
  `gpio_device_get_chip()` and `get_random_u32_below()`), plus the
  `gpio-sim` module (available from 5.17, included in Ubuntu kernels)

## Building

```sh
sudo apt install build-essential linux-headers-$(uname -r)
make
```

This produces `word_storage.ko`. `make clean` removes the build output.

## Setting up gpio-sim

Create a simulated chip with 8 lines and label it `sim_chip` (the module's
default chip name):

```sh
sudo modprobe gpio-sim
cd /sys/kernel/config/gpio-sim
sudo mkdir -p mychip/bank0
echo 8        | sudo tee mychip/bank0/num_lines
echo sim_chip | sudo tee mychip/bank0/label
echo 1        | sudo tee mychip/live

cat mychip/dev_name          # e.g. gpio-sim.0
cat mychip/bank0/chip_name   # e.g. gpiochip1
```

## Loading and unloading

```sh
sudo insmod word_storage.ko gpio_chip_name=sim_chip gpio_offset=0
sudo rmmod word_storage
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `gpio_chip_name` | `sim_chip` | Label of the GPIO chip to use |
| `gpio_offset` | `0` | Line number within that chip |

On load the module finds the chip by its label, requests the line as an input
and registers a threaded interrupt on its rising edge (the IRQ appears as
`WordIRQ` in `/proc/interrupts`).

## Using the device

`/dev/wordsDev` is only writable by root by default, so writes go through `sudo tee`.

```sh
# Replace the stored text (the device is opened with O_TRUNC)
echo "Hello FRITZ!" | sudo tee /dev/wordsDev > /dev/null

# Append to it (opened with O_APPEND)
echo "This is my assignment" | sudo tee -a /dev/wordsDev > /dev/null

# Read it back
sudo cat /dev/wordsDev
```

Behaviour:

- Opening the device for writing with `O_TRUNC` (the shell's `>`, or plain `tee`)
  clears the stored text; every `write()` appends. This means a file written in
  several chunks is stored in full.
- Text is split into words on spaces, tabs and newlines. The whole buffer is
  re-split after each write, so a word cut across two writes is still one word.
- The total stored text is limited to 64 KiB; larger writes fail with `EFBIG`.

## Triggering the GPIO

Drive the simulated line high and low through its `pull` attribute (adjust
`gpio-sim.0` / `gpiochip1` / `sim_gpio0` to match your setup):

```sh
P=/sys/devices/platform/gpio-sim.0/gpiochip0/sim_gpio0
echo pull-up   | sudo tee $P/pull    # rising edge -> a random word is logged
echo pull-down | sudo tee $P/pull
```

Each rising edge logs a line such as:

```
wordsDev: Randomly selected word: banana
```

If nothing is stored, it logs `wordsDev: No words to log` instead.

## Example session

`example.log` was produced with:

```sh
sudo dmesg -C
sudo insmod word_storage.ko gpio_chip_name=sim_chip gpio_offset=0
echo "Hello FRITZ!" | sudo tee /dev/wordsDev > /dev/null
echo "This is my assignment"     | sudo tee -a /dev/wordsDev > /dev/null
sudo cat /dev/wordsDev
for n in 1 2 3; do
  echo pull-up   | sudo tee $P/pull > /dev/null
  echo pull-down | sudo tee $P/pull > /dev/null
done
grep WordIRQ /proc/interrupts
sudo rmmod word_storage
sudo dmesg > example.log
```
