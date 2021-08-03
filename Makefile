.PHONY:	all
all:	respawn

.PHONY:	man
man:	respawn.man

CFLAGS = -Wall -Werror
respawn:	respawn.c

respawn.man:	respawn.1
	nroff -man $< >$@.tmp && mv $@.tmp $@
