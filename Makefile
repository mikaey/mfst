mfst: mfst.o base64.o state.o device.o
	gcc -g -o mfst mfst.o state.o base64.o device.o -lncurses -ludev -ljson-c

mfst.o: mfst.c mfst.h
	gcc -c -g -o mfst.o mfst.c

base64.o: base64.c base64.h
	gcc -c -g -o base64.o base64.c

state.o: state.c state.h mfst.h
	gcc -c -g -o state.o state.c

device.o: device.c device.h
	gcc -c -g -o device.o device.c
