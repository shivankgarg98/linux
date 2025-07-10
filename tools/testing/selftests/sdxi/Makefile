werror := -Werror
CFLAGS += -std=gnu17 -Og -Wall -Wextra $(werror) $(KHDR_INCLUDES)
CPPFLAGS += -I../../../include/

TEST_GEN_PROGS += simple_cdev
$(OUTPUT)/simple_cdev: lib.o

include ../lib.mk

# When I change lib.h, I want the files that include it to rebuild.
# This isn't handled automatically and I don't see how to communicate
# such dependencies to this build system. Use this hack for now:
.PHONY: *.c
