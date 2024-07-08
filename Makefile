mfst: mfst.o base64.o state.o device.o util.o crc32.o block_size_test.o
	gcc -g -o mfst mfst.o state.o base64.o util.o device.o crc32.o block_size_test.o -lncurses -ludev -ljson-c

mfst.o: mfst.c base64.h block_size_test.h crc32.h device.h mfst.h state.h util.h
	gcc -c -g -o mfst.o mfst.c

base64.o: base64.c base64.h
	gcc -c -g -o base64.o base64.c

state.o: state.c state.h mfst.h
	gcc -c -g -o state.o state.c

device.o: device.c device.h
	gcc -c -g -o device.o device.c

util.o: util.c util.h
	gcc -c -g -o util.o util.c

crc32.o: crc32.c crc32.h
	gcc -c -g -o crc32.o crc32.c

block_size_test.o: block_size_test.c block_size_test.h mfst.h util.h
	gcc -c -g -o block_size_test.o block_size_test.c
