TARGET = devicetree-parse

DEBUG   ?= 0
ARCH    ?= x86_64
SDK     ?= macosx

SYSROOT  := $(shell xcrun --sdk $(SDK) --show-sdk-path)
ifeq ($(SYSROOT),)
$(error Could not find SDK "$(SDK)")
endif
CLANG    := $(shell xcrun --sdk $(SDK) --find clang)
CC       := $(CLANG) -isysroot $(SYSROOT) -arch $(ARCH)

CFLAGS  = -O2 -Wall -Werror
LDFLAGS =

ifneq ($(DEBUG),0)
DEFINES += -DDEBUG=$(DEBUG)
endif

FRAMEWORKS =

SOURCES = devicetree-parse.c \
	  main.c

HEADERS = devicetree-parse.h

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(FRAMEWORKS) $(DEFINES) $(LDFLAGS) -o $@ $(SOURCES)

clean:
	rm -f -- $(TARGET)
