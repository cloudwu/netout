LUAINC = -I/usr/local/include
LUALIB = -L/usr/local/bin -llua53

CC= gcc
CFLAGS = -O2 -Wall

all : netout.dll

netout.dll : netout.c
	$(CC) -o $@ --shared $^ $(LUAINC) $(LUALIB) -lws2_32

clean :
	rm netout.dll