INCLUDE_CJSON=1
include ../common/config.mk
CFILES := $(filter-out ./vcinput.c,$(CFILES))

TARGET = kidui
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_ttf -lSDL_image -lSDL_rotozoom -pthread

all: $(TARGET) libvcinput.so

include ../common/commands.mk
include ../common/recipes.mk

libvcinput.so: vcinput.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $< -L../../lib -L/usr/local/lib \
		-ldl -lSDL -lSDL_image -lSDL_rotozoom
