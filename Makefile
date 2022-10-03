UNAME := $(shell uname)
CFLAGS += -I../sml/include/ -g -std=c99 -Wall -Wextra -pedantic
OBJS = sml_smartmeter.o 
LIBSML = ../sml/lib/libsml.a
INSTALL = /usr/bin/install
prefix = /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
libdir = ${exec_prefix}/lib
includedir = ${prefix}/include

LIBS = -lm -lmosquitto
ifneq ($(UNAME), Darwin)
LIBS += -luuid
endif

all: sml_smartmeter

sml_smartmeter: $(OBJS) $(LIBSML)
	$(CC) $(OBJS) $(LIBSML) $(LIBS) -o sml_smartmeter

%.o: %.c
	$(CC) $(CFLAGS) -c $^ -o $@

.PHONY: clean install uninstall
clean:
	@rm -f *.o
	@rm -f sml_smartmeter

install: sml_smartmeter
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) sml_smartmeter $(DESTDIR)$(bindir)

uninstall:
	@rm -f $(DESTDIR)$(bindir)/sml_smartmeter
