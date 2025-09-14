# Mikaey's Flash Stress Test
A program to stress test flash media to the point of failure -- and possibly beyond.  It was inspired by [`f3probe` by Michel Machado](https://github.com/AltraMayor/f3) and [`stressdisk` by Nick Craig-Wood](https://github.com/ncw/stressdisk) and is designed to be a program that combines the functionality of both of them (while improving on shortcomings with both of them).  It focuses primarily on SD media, but I don't see any reason why it couldn't be used for other types of media, like USB or NVMe flash drives...or even mechanical drives.

This program will:
* Attempt to detect whether a storage device is fake flash and determine the true size of the device
* Speed test the device and determine which SD card speed markings it would likely qualify for
* Stress test the device to see how long it will survive

[!(Animated screenshot showing the program running and stress testing the device)](mfst.mp4)

## WARNING
This program operates only on block devices (e.g., entire disks).  Further, it will overwrite every portion of your device with random data.  It makes no attempt to preserve any data on the device before it starts doing this.  **DO NOT USE THIS PROGRAM ON ANY DISK THAT HAS DATA THAT IS NOT PROPERLY BACKED UP.**  If you accidentally use it on a device that had data you cared about, you should consider that data permanently gone.  You have been warned.

## How to Build
I built this on a system running Ubuntu 20.04 -- these instructions *should* work on any system running that or a newer version.

First, install your prerequisites:
```
# sudo apt install git build-essential pkg-config autoconf uuid-dev libudev-dev libjson-c-dev libncurses-dev libmariadb-dev 
```

The download and build the program:
```
# git clone https://github.com/mikaey/mfst.git
# cd mfst
# ./configure
# make
```

## How to use
The simplest way to invoke this program would be with:

```
# sudo ./mfst <path_to_device>
```

For example, if the device you want to test is /dev/sdc, then you'd simply do:

```
# sudo ./mfst /dev/sdc
```

This program shows a progress display (using ncurses) so that you can stay appraised of what's going on.  However...depending on the quality, size, and speed of the device you're testing, how fast your reader is, how many devices you're testing simultaneously, etc., this can take days.  Or weeks.  Or months.  Be prepared to let it run.  If you're running it on a remote system, consider running it inside of `screen` or `tmux` so that it doesn't get killed if you get disconnected.

More on the various options below.

### Save stating
Endurance tests can take a *long* time.  As of the time I'm writing this, it's been two years since I started this project -- and I have cards that have been going through testing the entire time.  The longer a card keeps running, the chances that you'll have another failure (such as a power failure or a hardware failure on some other part of your machine) will go up.

That's why this program supports save states.  If the program is terminated for some reason, you can resume it from the start of the most recent round of testing.  To do this, add the `-t` option to your command line and specify where you want the save state to be saved.  For example:

```
# sudo ./mfst -t Kioxia_Exceria_G2_64GB_1_state.json /dev/sdc
```

If you need to restart the program, just give it the same `-t` option on the command line:

```
# sudo ./mfst -t Kioxia_Exceria_G2_64GB_1_state.json
```

**NOTE:**
* You don't need to specify the device when restarting the program in this way -- the save state has enough information for the program to automatically figure out which device was being tested (or alert you if it can't find the device).
* The device must complete at least one round of endurance testing before you can resume it from a save state.

## About the various tests
When testing a new device, the program goes through the following tests, in order.

### RNG Test
The first thing the program does is to run a 5-second test to see how fast your system can generate random data.  This is basically to try to make sure that your system can keep up with higher-speed cards during the speed tests.  If your system can't generate at least 90MB/sec of random data, you'll get a warning -- if you get this warning, just know that the results of the speed tests will probably indicate that the card is slower than it actually is.

If you have logging enabled, the results of this test are logged to the log file.

### Capacity Test
Next, the program will try to determine the true size of the device.  It does this by writing random data to several different parts of the device, then reading that data back and comparing it to what was written.  If everything checks out, then the media is considered to be genuine.  If any discrepancies are found, the program probes further to determine exactly where the "good" portion of the media ends and the "bad" portion begins.

Regardless of the outcome, the program displays the size reported by the device as well as its true size.  (Note that right now, this program doesn't have a way to see if the card is *bigger* than reported -- it can only try to determine if it's the same size or smaller than reported.)

The results of this test are shown on the screen.  If you have logging enabled, the results are also logged to the log file.

### Optimal Block Size Test
This is an optional test that is turned off by default.  You can enable it with the `-b` option.  If you enable it, it is run *before* the fake flash test.

On flash media, the write speeds get faster the more data you write at one time -- up to a point.  For example, it's faster to write 4,096 bytes at a time instead of 512 bytes.  This is because the controller generally has to reprogram an entire block at a time -- which involves erasing all the bits in it and then reprogramming all of them.  If you're writing less than the size of a block, it has to read in what's already there first, apply your changes, then re-write the new data out to the block.  If you can write in multiples of the block size, then the controller can just reprogram the entire block without having to worry about saving what's there -- which results in a faster write operation.

But how big is a block?  It varies by device, and USB devices don't always give you a good way of discovering this.

This test doesn't specifically try to determine what the device's block size is -- it simply tries several different block sizes to see which one is the fastest.  To do this, it writes 256 MB of random data to the device -- multiple times, each using a different block size -- and timing it to see how long it takes.  It picks the option that is the best balance of small *and* fast.  That block size is then used for writes throughout most of the rest of the program.

Usually this test isn't necessary because the kernel will tell us the maximum number of sectors it will allow per request, and we can simply use that value.  However, since I wrote this test before I discovered that this information was available, I decided to leave it in as an option.

### Speed Tests
SD cards usually have a number of markings on them indicating how well they perform.  However, cards don't always perform well enough to qualify for these markings.  To help you determine whether the device is misadvertising its speeds, the program runs four speed tests:

* A sequential read test
* A sequential write test
* A random read test
* A random write test

Each of these tests is run for 30 seconds.

**NOTE:** The SD Association prescribes specific methods for determining whether a card qualifies for a given performance mark.  This program does **NOT** follow those methods.  The results of this test should not be used to indicate that it does or does not qualify for a given performance mark!

The results of this test are shown on the screen.  If you have logging enaabled, the results are also logged to the log file.

### Endurance Test
Finally, the program moves onto the last -- and longest -- test, the endurance test.  In this test, the program overwrites the entire device with random data.  It then reads back the entire contents of the device and compares it to what was written to it, and flags any sectors that could not be read back or whose contents did not match what was written.  It then repeats this process -- overwriting the entire card and reading it back -- over and over again, until either (a) the card stops working entirely, or (b) 50% or more of the sectors on the device have been flagged as "bad".

During each pass, the device is written to, and read back, in a random order -- this is done to minimize the effects of any caching that the card might be employing on the overall results of the endurance test.

Flash devices vary in how they handle failing sectors.  Some will set themselves to be read-only.  Some will disable themselves entirely.  Some might pretend like nothing is wrong and simply return wrong data.  (Most devices have some extra space set aside for this inevitability -- and will start silently swapping in parts of that space for any sectors that have failed -- but we're concerned with what the device does once it has exhausted this extra space.)  If a device falls into this last category, then the program will keep testing the device even after it starts finding bad sectors on the device.  It will keep going until at least half of the sectors on the device have been flagged as "bad".  Once it hits that point, it will show you:
* How many read/write cycles were completed before the first failure appeared
* How many read/write cycles were completed before 0.1% of the sectors on the device failed
* How many read/write cycles were completed before 1% of the sectors on the device failed
* How many read/write cycles were completed before 10% of the sectors on the device failed
* How many read/write cycles were completed before 25% of the sectors on the device failed
* How many read/write cycles were completed before 50% of the sectors on the device failed

A few things to note:
* Occasional errors tend to be a fact of life with flash media.  Sometimes, it's not even the device's fault.  I've added code to mitigate against one particular type of error, but there are still others.  If a device has a few bad sectors every once in a great while, it's not necessarily cause for alarm: it might still go several thousand more read/write cycles before any more errors appear.  When a device starts to show errors more frequently, that's usually a better indicator that it's going to fail -- which is why I, personally, usually put more stock in how many read/write cycles a device is able to complete before 0.1% of the sectors fail.
* If a device reaches one of these milestones, but then fails for another reason, it will still display how many read/write cycles were completed before each of the milestones that it *was* able to reach (if any).
* If a device is disconnected for some reason, this program is designed to wait for it to reconnect.  If it reconnects, it will automatically resume from where it left off.  If the device disables itself entirely, however, you will need to kill the program (Ctrl+C will do the trick) instead.

#### SQL Logging
If provided with credentials to a MySQL or MariaDB server, the program will periodically (every 30 seconds) log its progress to the given MySQL/MariaDB server during the endurance test.  A sample schema is included in `mfst.sql`.  This can make it easier to monitor the status of multiple cards that are being tested by different copies of the program.

To use SQL logging, pass the `--dbhost`, `--dbuser`, `--dbpass`, `--dbname`, and `--cardname` options.  (Once the card has been registered in the database, `--cardname` can be omitted on future invocations of the program -- but `--dbhost`, `--dbuser`, `--dbpass`, and `--dbname` still need to be passed.)

The logged information includes:
* The name of the card
* The size of the device
* The current read/write cycle number
* I/O rate since the last update
* What stage of the endurance test the program is in (e.g., reading, writing, waiting for a device to reconnect, ending, etc.)
* A blob showing which sectors have been written during the current pass, which sectors have been read in the current pass, and which sectors are flagged as "bad"

A basic web application that displays this data is included in the `webmonitor` folder.

## Command-Line Arguments

| Option                            | Description |
|-----------------------------------|-------------|
| `-s file`/`--stats-file file`     | During the stress test, stats are periodically written -- in CSV format -- to `file`.  The default is to write stats once every 60 seconds, but you can change this with the `-i` option.  The stats include the number of read/write cycles completed so far, the number of bytes read/written during the last interval, the number of new bad sectors discovered during the last interval, and the average read/write rate. |
| `-l file`/`--log-file file`       | Write log messages out to `file`. **NOTE:** Log files can get big (on the orders of gigabytes or even hundreds of gigabytes)! |
| `-b`/`--probe-for-block-size`     | Runs the optimal block size test (see above for more information). |
| `-i secs`/`--stats-interval secs` | Changes the interval at which stats are written to the stats file.  The default is once every 60 seconds. |
| `-n`/`--no-curses`                | Don't display the curses UI.  When this option is enabled, log messages are printed to standard output instead.  Note that this option is automatically enabled if (a) the program detects that standard output isn't a tty (for example, if you're redirecting output to a file), or if the screen is too small to hold the UI. |
| `--this-will-destroy-my-device`   | Upon startup, the program displays a warning message to let you know that your device is going to be DESTROYED.  It then waits 15 seconds to give you a chance to abort if you change your mind.  If you know what you're doing and you'd rather not see this warning, you can use this option to suppress it. |
| `-f file`/`--lockfile file`       | If the program detects that another copy of the program is running speed-critical tests (such as the speed test or the optimal block size test), the program will stop what it's doing and yield to the other copy.  This is done because this program is pretty I/O intensive, and this frees up bandwidth on the PCI/USB buses for the other program to use.  This is done through the use of a lockfile -- and for this feature to work, all copies of the program must be using the same lockfile.  The default is to use a file called `mfst.lock` in the program's working directory.  If you're running the program from another folder than the others, you'll need to pass this option and give it the path to the lockfile that the other copies of the program are using. |
| `-e count`/`--sectors count`      | Assume that the device is `count` sectors in size.  If this option is used on a new device, the capacity test is skipped, and this value is used instead.  This option has no effect when resuming the program from a save state. |
| `--force-device device_name`      | When resuming the program from a save state, force the program to use the given device.  This option is useful for devices where the media has become extremely corrupted and the program is not automatically able to figure out which device was being tested.  This option has no effect when testing a new device.  **Use this option with caution!** |
| `--dbhost hostname`               | The hostname of the MySQL or MariaDB host to connect to. |
| `--dbuser username`               | The username to use when connecting to the MySQL or MariaDB host. |
| `--dbpass password`               | The password to use when connecting to the MySQL or MariaDB host. |
| `--dbname database_name`          | The name of the database to use when connecting to the MySQL or MariaDB host. |
| `--cardname name`                 | The name of the card, as you want it to be registered in the database.  (This is descriptive and only for your own use.  Make sure to enclose the name in quotes if it includes spaces or special characters!) |
| `--cardid id`                     | Force the program to use the given ID when logging information on this card to the database.  (You generally shouldn't need to use this option -- the program will figure it out on its own.  However, if you do provide it, keep in mind that the program will be expecting the value of the `id` column from `cards` or `consolidated_sector_maps` table.) |
| `-h`/`--help`                     | Display the program's help text. |

## Things I Want To Do
* Read CID/CSD (if available) and print it to the log.  Possibly decode both of them as well.
* Add detection for wraparound flash.  I haven't seen any of these yet, so I don't know how prevalent they are (or if they even exist) or how they work.
* Make the program interactive
* Make the program multithreaded (so that a single copy of the program can test multiple cards at the same time)
