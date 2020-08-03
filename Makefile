
CXXFLAGS=-std=c++17 -Wall -O2 -pthread
LDLIBS=

BINS=lb-simple lb-queued ud-simple ud-queued

LINK.o=$(LINK.cc)

.PHONY: all
all: $(BINS)

lb-queued.o lb-simple.o ud-queued.o ud-simple.o: fd_queue.h ping_pong.h

lb-queued: lb-queued.o
lb-simple: lb-simple.o
ud-queued: ud-queued.o
ud-simple: ud-simple.o

.PHONY: clean
clean:
	rm -fv *.o $(BINS)
