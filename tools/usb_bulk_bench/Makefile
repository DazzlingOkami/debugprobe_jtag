CC = gcc
LIBUSB_DIR = ./libusb-1.0
CFLAGS = -O2 -Wall -Wextra
LDLIBS = -L$(LIBUSB_DIR) -lusb-1.0

.PHONY: all clean

all: usb_bulk_bench.c
	$(CC) $(CFLAGS) -I$(LIBUSB_DIR) -o usb_bulk_bench $< $(LDLIBS)

clean:
	rm -f usb_bulk_bench usb_bulk_bench.exe
