APXS ?= apxs
PKG_CONFIG ?= pkg-config

CFLAGS_EXTRA := -Wall -Wextra -Wno-unused-parameter \
	$(shell $(PKG_CONFIG) --cflags sqlite3)

LIBS_EXTRA := $(shell $(PKG_CONFIG) --libs sqlite3)

SOURCES = \
	mod_todotxt.c \
	todotxt_api.c \
	todotxt_parser.c \
	todotxt_sqlite.c

all:
	$(APXS) -c $(CFLAGS_EXTRA) $(SOURCES) $(LIBS_EXTRA)

install:
	$(APXS) -i -a -n todotxt .libs/mod_todotxt.so

clean:
	rm -rf .libs *.o *.lo *.la *.slo
