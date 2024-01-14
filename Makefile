
CFLAGS = -O2 -Wall -Wextra -std=c99 -pedantic -Wno-unused
# CLFAGS += -fsanitize=address
APPNAME = toumapet

ifneq ($(SDL),)
CFLAGS += -DUSE_SDL=$(SDL)
ifeq ($(SDL),1)
LIBS = -lSDL
endif
ifeq ($(SDL),2)
LIBS = -lSDL2
endif
endif
ifeq ($(X11),1)
CFLAGS += -DUSE_X11=1
LIBS = -lX11
endif
ifeq ($(GDI),1)
CFLAGS += -DUSE_GDI=1
LIBS = -lGDI32 -lwinmm
endif

.PHONY: all clean
all: $(APPNAME)

clean:
	$(RM) $(APPNAME)

$(APPNAME): $(APPNAME).c window.h
	$(CC) -s $(CFLAGS) $(EXTRA) -o $@ $< $(LIBS)

