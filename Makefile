.PHONY:	all
all:	respawn

.PHONY:	man
man:	respawn.man

.PHONY:	lib
lib:	library.a

.PHONY:	clean
clean:
	$(RM) *.o
	$(RM) library.a

CFLAGS = -Wall -Werror -D_GNU_SOURCE -Ilib/
respawn:	respawn.c library.a

LIBOBJS = $(patsubst %.c,%.o,$(wildcard lib/*.c))
ARFLAGS = crvs
library.a:	$(foreach o,$(LIBOBJS),library.a($o))

respawn.man:	respawn.1
	nroff -man $< >$@.tmp && mv $@.tmp $@
