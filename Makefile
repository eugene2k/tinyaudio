LIBS:= libavcodec libswresample libavutil libavformat libpulse libpulse-simple dbus-1

CFLAGS += -g -Wall -Wextra $(shell pkg-config --cflags ${LIBS})
LDLIBS += $(shell pkg-config --libs ${LIBS})

all:
	@mkdir -p build
	${CC} ${CFLAGS} -o build/tinyaudio src/main.c ${LDLIBS}

release: CFLAGS += -Os
release: all

clean:
	-rm -r build

install:
	install -D -s build/tinyaudio ${DESTDIR}/usr/bin/tinyaudio
