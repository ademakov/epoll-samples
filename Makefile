
CXXFLAGS=-std=c++17 -Wall -O2 -pthread
LDLIBS=

BINS=lb-sample ud-sample
HEADERS=fd_queue.h ping_pong.h

LINK.o=$(LINK.cc)

.PHONY: all
all: $(BINS)

lb-sample.o: $(HEADERS)
ud-sample.o: $(HEADERS)

lb-sample: lb-sample.o
ud-sample: ud-sample.o

.PHONY: clean
clean:
	rm -fv *.o $(BINS)
