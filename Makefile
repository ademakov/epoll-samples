
CXXFLAGS=-std=c++17 -Wall -O2 -pthread
LDLIBS=

LINK.o=$(LINK.cc)

.PHONY: all
all: sample

sample: sample.o

.PHONY: clean
clean:
	rm -fv *.o sample
