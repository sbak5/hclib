TARGETS=gups gups-xor gups-xor-pthreads gups-xor-pthreads-contexts gups-pthreads gups-pthreads-contexts
FLAGS=-O2 -g

all: $(TARGETS)

gups: gups.c
	 $(OPENSHMEM_INSTALL)/bin/oshcc -o $@ $(FLAGS) $^

gups-pthreads: gups.c
	 $(OPENSHMEM_INSTALL)/bin/oshcc -o $@ $(FLAGS) $^ -DUSE_PTHREADS -lpthread

gups-pthreads-contexts: gups.c
	 $(OPENSHMEM_INSTALL)/bin/oshcc -o $@ $(FLAGS) $^ -DUSE_PTHREADS -DUSE_CONTEXTS -lpthread

gups-xor: gups.c
	 $(OPENSHMEM_INSTALL)/bin/oshcc -o $@ $(FLAGS) $^ -DUSE_BXOR_ATOMICS

gups-xor-pthreads: gups.c
	 $(OPENSHMEM_INSTALL)/bin/oshcc -o $@ $(FLAGS) $^ -DUSE_BXOR_ATOMICS -DUSE_PTHREADS -lpthread

gups-xor-pthreads-contexts: gups.c
	 $(OPENSHMEM_INSTALL)/bin/oshcc -o $@ $(FLAGS) $^ -DUSE_BXOR_ATOMICS -DUSE_PTHREADS -DUSE_CONTEXTS -lpthread

clean:
	rm -f $(TARGETS)
