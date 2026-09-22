KDIR ?= /lib/modules/$(shell uname -r)/build

CLANG_FORMAT ?= clang-format
CHECKPATCH ?= $(KDIR)/scripts/checkpatch.pl

.PHONY: all make clean load unload format check boardctl

all: make boardctl

load: make
	sudo insmod ./virtual_board.ko

unload:
	sudo rmmod virtual_board

make:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

boardctl: tools/boardctl

tools/boardctl: tools/boardctl.c include/virtual_board_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -O2 $< $(LDFLAGS) -o $@

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) tools/boardctl

format:
	$(CLANG_FORMAT) -i -style='{BasedOnStyle: LLVM, IndentWidth: 8, TabWidth: 8, UseTab: Always, BreakBeforeBraces: Linux, ColumnLimit: 80, Cpp11BracedListStyle: false, BreakStringLiterals: false}' src/*.c src/*.h include/*.h tools/*.c

check:
	@result=0; \
  	for file in src/*.c src/*.h include/*.h tools/*.c Makefile Kbuild; do \
  		if [ -s "$$file" ]; then \
  			"$(CHECKPATCH)" --no-tree --file "$$file" || result=1; \
  		fi; \
  	done; \
  	exit $$result
