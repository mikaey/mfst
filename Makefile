mfst: mfst.o base64.o state.o device.o util.o crc32.o sql.o lockfile.o ncurses.o block_size_test.o rng.o messages.o device_speed_test.o device_testing_context.o
	gcc -g -o mfst mfst.o state.o base64.o util.o device.o crc32.o sql.o lockfile.o ncurses.o block_size_test.o rng.o messages.o device_speed_test.o device_testing_context.o -pthread -lncurses -ludev -ljson-c -lmariadb -luuid

mfst.o: mfst.c base64.h block_size_test.h crc32.h device.h lockfile.h messages.h mfst.h ncurses.h rng.h sql.h state.h util.h
	gcc -c -g -o mfst.o mfst.c

base64.o: base64.c base64.h
	gcc -c -g -o base64.o base64.c

state.o: state.c state.h messages.h mfst.h
	gcc -c -g -o state.o state.c

device.o: device.c device.h messages.h mfst.h
	gcc -c -g -o device.o device.c

util.o: util.c util.h
	gcc -c -g -o util.o util.c

crc32.o: crc32.c crc32.h
	gcc -c -g -o crc32.o crc32.c

sql.o: sql.c sql.h messages.h mfst.h
	gcc -c -g -o sql.o sql.c

lockfile.o: lockfile.c lockfile.h messages.h mfst.h
	gcc -c -g -o lockfile.o lockfile.c

ncurses.o: ncurses.c ncurses.h mfst.h
	gcc -c -g -o ncurses.o ncurses.c

block_size_test.o: block_size_test.c block_size_test.h lockfile.h messages.h mfst.h ncurses.h rng.h util.h
	gcc -c -g -o block_size_test.o block_size_test.c

rng.o: rng.c rng.h
	gcc -c -g -o rng.o rng.c

messages.o: messages.c messages.h
	gcc -c -g -o messages.o messages.c

device_speed_test.o: device_speed_test.c device_speed_test.h messages.h mfst.h ncurses.h rng.h util.h device_testing_context.h
	gcc -c -g -o device_speed_test.o device_speed_test.c

device_testing_context.o: device_testing_context.c device_testing_context.h mfst.h
	gcc -c -g -o device_testing_context.o device_testing_context.c
clean:
	rm mfst *.o
