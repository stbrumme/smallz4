# Makefile for smallz4 (see https://create.stephan-brumme.com/smallz4/ )
#
# This makefile was only tested on my Linux system
#
# You can define EXTRAFLAGS via command-line, e.g.: EXTRAFLAGS="-static" make
# CLang's binaries are typically a bit smaller

# compile both programs
PROGRAMS = smallz4 smallz4cat

# compiler
CC  = gcc
CPP = g++

# compiler flags
# add -m32 if you want 32bit binaries (or add to EXTRAFLAGS via command-line)
CCFLAGS   = -O3 -Wall -pedantic -s -std=c99
CPPFLAGS  = -O3 -Wall -pedantic -s

# you need dietlibc for super-small portable (=static) binaries: https://www.fefe.de/dietlibc/
# compiler flags are explained here: http://ptspts.blogspot.com/2013/12/how-to-make-smaller-c-and-c-binaries.html
# NOTE: -Os instead of -O3 saves about 100 bytes (CLang) or 3kb (GCC) - but decompression speed is >20% worse
TINYFLAGS = $(CCFLAGS) -s -std=c99 -static -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wl,--gc-sections 
TINYFLAGS+= -fno-ident
TINYCAT   = tiny-smallz4cat$(EXTRAFLAGS)

# build smallz4 and smallz4cat
default: $(PROGRAMS)
all: default

# build only smallz4
smallz4: smallz4.h smallz4.cpp Makefile
	$(CPP) $(CPPFLAGS) $(EXTRAFLAGS) smallz4.cpp  -o smallz4

# build only smallz4cat
smallz4cat: smallz4cat.c Makefile
	$(CC)  $(CCFLAGS)  $(EXTRAFLAGS) smallz4cat.c -o smallz4cat

# compile static binary of smallz4cat, use super-aggressive settings (tuned for my development machine setup)
tiny-smallz4cat: smallz4cat.c Makefile
	@diet clang $(TINYFLAGS) $(EXTRAFLAGS) smallz4cat.c -o $(TINYCAT)
	@strip -S --strip-unneeded -R .comment -R .note.gnu.build-id -R .note.ABI-tag -R .got.plt $(TINYCAT)
	@wc -c $(TINYCAT)

# delete binaries
clean:
	-rm -f $(PROGRAMS) $(TINYCAT)
