CC      = gcc
CFLAGS  = -Wall -Wextra $(shell pkg-config --cflags gstreamer-1.0)
LIBS    = $(shell pkg-config --libs gstreamer-1.0)
TARGET  = rover-stream

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
