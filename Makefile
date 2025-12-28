all:
	-mkdir build 2>/dev/null
	cc -Wall -g -o build/tinyaudio src/main.c $(shell pkg-config --libs --cflags libavcodec libswresample libavutil libavformat libpulse libpulse-simple dbus-1)
clean:
	-rm -r build

install:
	install -D -s build/tinyaudio /usr/bin/tinyaudio
