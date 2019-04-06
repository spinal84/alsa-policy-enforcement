CC=gcc

CFLAGS=-Wall -Werror

all: alsaped test

alsaped: alsaped.o logging.o alsaif.o config.o dbusif.o control.o
	gcc `pkg-config --libs alsa,glib-2.0,dbus-glib-1` -lm \
	    $^ -o $@

test: test.o logging.o alsaif.o config.o dbusif.o control.o
	gcc `pkg-config --libs alsa,glib-2.0,dbus-glib-1` -lm \
	    $^ -o $@

alsaif.o: alsaif.c alsaif.h options.h logging.h
	$(CC) -c $(CFLAGS) $< `pkg-config --cflags glib-2.0`

config.o: config.c config.h control.h options.h
	$(CC) -c $(CFLAGS) $< `pkg-config --cflags glib-2.0`

control.o: control.c control.h alsaif.h options.h dbusif.h logging.h
	$(CC) -c $(CFLAGS) $< `pkg-config --cflags glib-2.0`

dbusif.o: dbusif.c dbusif.h control.h options.h logging.h
	$(CC) -c $(CFLAGS) $< `pkg-config --cflags glib-2.0,dbus-glib-1`

alsaped.o: alsaped.c
	$(CC) -c $(CFLAGS) $< `pkg-config --cflags glib-2.0`

logging.o: logging.c logging.h options.h
	$(CC) -c $(CFLAGS) $<

test.o: test.c logging.h
	$(CC) -c $(CFLAGS) $< `pkg-config --cflags glib-2.0`

clean:
	rm -f *.o test alsaped
