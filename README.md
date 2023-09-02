# Mikaey's Flash Stress Test
A program to stress test flash media to the point of failure -- and possibly beyond.  It was inspired by `f3probe` by Michel Machado and `stressdisk` by Nick Craig-Wood and is designed to be a program that combines the functionality of both of them (while improving on shortcomings with both of them).  It focuses primarily on SD media, but I don't see any reason why it couldn't be used for other types of media, like USB or NVMe flash drives...or even mechanical drives.

This program will:
* Attempt to detect whether a storage device is fake flash and determine the true size of the device
* Speed test the device and determine which SD card speed markings it would qualify for
* Stress test the device to see how many read/write cycles it will survive until it fails

## WARNING
This program operates only on block devices (e.g., entire disks).  Further, it will overwrite every portion of your device with random data.  It makes no attempt to preserve any data on the device before it starts doing this.  **DO NOT USE THIS PROGRAM ON ANY DISK THAT HAS DATA THAT IS NOT PROPERLY BACKED UP.**  If you accidentally use it on a device that had data you cared about, you should consider that data permanently gone.  You have been warned.

## How to Build
`gcc -o mfst mfst.c -lncurses`

## How to use
The simplest way to invoke this program would be with:

`# sudo ./mfst <path_to_device>`

For example, if the device you want to test is /dev/sdc, then you'd simply do:

`# sudo ./mfst /dev/sdc`

This program shows a progress display (using ncurses) so that you can stay appraised of what's going on.  However...depending on the quality, size, and speed of the device you're testing, how fast your reader is, how many devices you're testing simultaneously, etc., this can take days.  Or weeks.  Or months.  Be prepared to let it run.  If you're running it on a remote system, consider running it inside of `screen` or `tmux` so that it doesn't get killed if you get disconnected.

More on the various options below.

## About the various tests
### Fake Flash Test
The first test is to determine the true size of the device.  It does this by writing pseudorandom data to several different parts of the device (including the very beginning and very end of the device), reading that data back, and comparing it to what was written.  If everything checks out, then the media is considered to be genuine.  If any discrepancies are found, the program probes further to determine exactly where the "good" portion of the media ends and the "bad" portion begins.

Regardless of the outcome, the program displays the size reported by the device as well as the size discovered as a result of probing the device.

### Speed Tests
SD cards usually have a number of markings on them indicating how well they perform.  However, fake flash may not live up to these markings either.  To help you determine whether the device is misadvertising its speeds, the program runs four speed tests:

* A sequential read test
* A sequential write test
* A random read test
* A random write test

Each of these tests is run for 30 seconds.

For the sequential read and sequential write tests, the program starts reading from and writing to the beginning of the device -- in a sequential fashion -- for 30 seconds.  On each read/write attempt, the program tries to read or write as much data as it can at one time to try to get the best results.  It tracks how much data was read or written and divides it by 30 to determine the sequential read or write speed.

For the random read and random write tests, the program picks a random location on the device and attempts to read or write 4KB (4,096 bytes) of data.  It then moves to another random location on the device and does the same thing.  It repeats this for 30 seconds.  The results of these tests are measured in IOPS/s (input/output operations per second).  We use 4KB as our read and write size as this is what's required for the A1 and A2 markings.  Similarly, we measure the result in IOPS/s because the requirements for the A1 and A2 markings are measured in IOPS/s.  (The program also shows the results in bytes/sec by simply multiplying the results by 4,096.)

### Stress Test
Finally, the program moves onto the last -- and longest -- test, the stress test.  The idea is fairly simple: we overwrite the entire device with pseudorandom data, then read it back to see if it matches.  Then, we repeat this process until either (a) some error prevents us from doing anything else with the device, or (b) 50% of the sectors on the device have failed verification.

Flash devices vary in how they handle failing sectors.  Some will set themselves to be read-only.  Some will disable themselves entirely.  Some might pretend like nothing is wrong and simply return wrong data.  (Most devices have some extra space set aside for this inevitability -- and will start silently swapping in parts of that space for any sectors that have failed -- but we're concerned with what the device does once it has exhausted this extra space.)  If a device is falls into this last category, then the program will keep testing the device even after it starts finding bad sectors on the device.  It will keep going until at least half of the sectors on the device have been flagged as "bad".  Once it hits that point, it will show you:
* How many read/write cycles were completed before the first failure appeared
* How many read/write cycles were completed before 10% of the sectors on the device failed
* How many read/write cycles were completed before 25% of the sectors on the device failed
* How many read/write cycles were completed before 50% of the sectors on the device failed

(Note that if a device gets to one of these milestones, but then runs into a different type of error, it will still display how many read/write cycles were completed before each of the milestones it *was* able to reach.)

### Optimal Block Size Test
This is an optional test that is turned off by default.  You can enable it with the `-b` option.  If you enable it, it is run *before* the fake flash test.

On flash media, the write speeds get faster the more data you write at one time -- up to a point.  For example, it's faster to write 4,096 bytes at a time instead of 512 bytes.  This is because the controller generally has to reprogram an entire block at a time -- which involves erasing all the bits in it and then reprogramming all of them.  If you're writing less than the size of a block, it has to read in what's already there first, apply your changes, then re-write the new data out to the block.  How big is a block?  Who knows...  But -- if you can write in multiples of the block size, then the controller can just reprogram the affected bits without worrying about reading them back in first.

This test doesn't specifically try to determine what the device's block size is -- it simply tries several different block sizes to see which one is the fastest.  To do this, it writes 256 MB of random data to the device -- multiple times, each using a different block size -- and timing it to see how long it takes.  It picks the option that is the best balance of small *and* fast.  That block size is then used for writes throughout most of the rest of the program.

Usually this test isn't necessary because the kernel will tell us the maximum number of sectors it will allow per request, and we can simply use that value.  However, since I wrote this test before I discovered that this information was available, I decided to leave it in as an option.

## Command-Line Arguments

| Option                            | Description |
|-----------------------------------|-------------|
| `-s file`/`--stats-file file`     | During the stress test, stats are periodically written -- in CSV format -- to `file`.  The default is to write stats once every 60 seconds, but you can change this with the `-i` option.  The stats include the number of read/write cycles completed so far, the number of bytes read/written during the last interval, the number of new bad sectors discovered during the last interval, and the average read/write rate. |
| `-l file`/`--log-file file`       | Write log messages out to `file`. |
| `-b`/`--probe-for-block-size`     | Runs the optimal block size test (see above for more information). |
| `-i secs`/`--stats-interval secs` | Changes the interval at which stats are written to the stats file.  The default is once every 60 seconds. |
| `-n`/`--no-curses`                | Don't display the curses UI.  When this option is enabled, log messages are printed to standard output instead.  Note that this option is automatically enabled if (a) the program detects that standard output isn't a tty (for example, if you're redirecting output to a file), or if the screen is too small to hold the UI. |
| `--this-will-destroy-my-device`   | Upon startup, the program displays a warning message to let you know that your device is going to be DESTROYED.  It then waits 15 seconds to give you a chance to abort if you change your mind.  If you know what you're doing and you'd rather not see this warning, you can use this option to suppress it. |
| `-f file`/`--lockfile file`       | If the program detects that another copy of the program is running speed-critical tests (such as the speed test or the optimal block size test), the program will stop what it's doing and yield to the other copy.  This is done because this program is pretty I/O intensive, and this frees up bandwidth on the PCI/USB buses for the other program to use.  This is done through the use of a lockfile -- and for this feature to work, all copies of the program must be using the same lockfile.  The default is to use a file called `mfst.lock` in the program's working directory.  If you're running the program from another folder than the others, you'll need to pass this option and give it the lockfile that the other programs are using. |

## Things I Want To Do
* Read CID/CSD (if available) and print it to the log.  Possibly decode both of them as well.
* Support hot removal/re-add of devices if they get disconnected.
