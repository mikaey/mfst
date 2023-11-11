mfst: mfst.o base64.o
	gcc -g -o mfst mfst.o base64.o -lncurses -ludev -ljson-c

mfst.o: mfst.c mfst.h
	gcc -c -g -o mfst.o mfst.c

base64.o: base64.c base64.h
	gcc -c -g -o base64.o base64.c
