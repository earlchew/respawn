# Monitor, and Restart a Process

**respawn** monitors a single process, and restarting it on
failure. It differs from service supervision suites such
as **s6** and **daemontools** in that retries can be restricted
to program failure, and that the behaviour is specified completely
on the command line.

## Description

**respawn** is inspired by **autossh** which behaves similarly,
but is focussed only **ssh** and secure tunnels. **respawn** can
support similar use cases, but is intended to have more general
applicability.

## Getting Started

### Dependencies

**respawn** is implemented entirely in Posix C, with **libc** as its only
dependency. The program is known to compile on:
* Mac OS
* Linux

### Installing

**respawn** is compiled from source using the accompanying `Makefile`.

### Executing program

Documentation is provided in the accompanying `respawn.man` file
which is created from using `man` target in the `Makefile`.
