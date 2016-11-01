PROGRAMS=smallz4 smallz4cat
FLAGS=-O3 -Wall -s
CC=gcc
CPP=g++

# I use EXTRAFLAGS via command-line, e.g.: EXTRAFLAGS="-static" make

default: $(PROGRAMS)
all: default

smallz4: smallz4.h smallz4.cpp
	$(CPP) $(FLAGS) $(EXTRAFLAGS) smallz4.cpp -o smallz4

smallz4cat: smallz4cat.c
	$(CC) $(FLAGS) $(EXTRAFLAGS) smallz4cat.c -o smallz4cat

clean:
	-rm -f $(PROGRAMS)
