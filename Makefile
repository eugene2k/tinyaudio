FLAGS = -O2 -s

all:
	@mkdir -p build
	cc -Wall ${FLAGS} -o build/tinyaudio src/main.c $(shell pkg-config --libs --cflags libavcodec libswresample libavutil libavformat libpulse libpulse-simple dbus-1)

debug: FLAGS:=-g
debug: all

clean:
	-rm -r build

install:
	install -D -s build/tinyaudio ${DESTDIR}/usr/bin/tinyaudio
