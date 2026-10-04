PREFIX = /usr/local
CC = gcc
CFLAGS = -O3 -Wall
LIBS = -lX11

SRC = hmwm.c
OBJ = ${SRC:.c=.o}

all: hmwm

micro_twm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LIBS}

.c.o:
	${CC} -c ${CFLAGS} $<

clean:
	rm -f hmwm ${OBJ}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f hmwm ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/hmwm

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/hmwm

.PHONY: all clean install uninstall