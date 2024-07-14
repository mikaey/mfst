mfst: mfst.o base64.o state.o device.o util.o crc32.o lockfile.o ncurses.o block_size_test.o rng.o
	gcc -g -o mfst mfst.o state.o base64.o util.o device.o crc32.o lockfile.o ncurses.o block_size_test.o rng.o -lncurses -ludev -ljson-c -luuid

mfst.o: mfst.c base64.h block_size_test.h crc32.h device.h lockfile.h mfst.h ncurses.h rng.h state.h util.h
       gcc -c -g -o mfst.o mfst.c

base64.o: base64.c base64.h
	gcc -c -g -o base64.o base64.c

state.o: state.c state.h mfst.h
	gcc -c -g -o state.o state.c

device.o: device.c device.h mfst.h
	gcc -c -g -o device.o device.c

util.o: util.c util.h
	gcc -c -g -o util.o util.c

crc32.o: crc32.c crc32.h
	gcc -c -g -o crc32.o crc32.c

lockfile.o: lockfile.c lockfile.h mfst.h
	gcc -c -g -o lockfile.o lockfile.c

ncurses.o: ncurses.c ncurses.h mfst.h
	gcc -c -g -o ncurses.o ncurses.c

rng.o: rng.c rng.h
	gcc -c -g -o rng.o rng.c

block_size_test.o: block_size_test.c block_size_test.h lockfile.h mfst.h util.h
	gcc -c -g -o block_size_test.o block_size_test.c
