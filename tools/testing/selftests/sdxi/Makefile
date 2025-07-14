werror := -Werror
CFLAGS += -std=gnu17 -Og -Wall -Wextra $(werror) $(KHDR_INCLUDES)
CPPFLAGS += -I../../../include/ -Ilib/

TEST_GEN_PROGS += context copy device nop

# Only for internal PASID PMR testing
TEST_GEN_MODS_DIR := pteditor

include ../lib.mk

HOSTAR ?= ar

LIBDIR	:= $(OUTPUT)/lib
LIB	:= $(LIBDIR)/lib.a

LIBDEPS := \
	Makefile \
	lib/compat.h \
	lib/descriptor.h \
	lib/fixture-context.h \
	lib/fixture-device.h \
	lib/lib.h \
	lib/ptedit.h

# Library
LIBSRC	:= context.c lib.c
LIBSRC	+= ptedit.c
LIBOBJ	:= $(LIBSRC:%.c=$(LIBDIR)/%.o)
EXTRA_CLEAN += $(LIBOBJ) $(LIB)

$(LIB): $(LIBOBJ)
	$(HOSTAR) rcs $@ $^

$(LIBDIR)/%.o: ./lib/%.c $(LIBDEPS)
	mkdir -p $(LIBDIR)
	$(CC) $< $(CFLAGS) $(CPPFLAGS) -o $@ -c

$(TEST_GEN_PROGS): $(LIB)
