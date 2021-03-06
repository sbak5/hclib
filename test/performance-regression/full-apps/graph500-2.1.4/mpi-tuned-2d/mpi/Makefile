# Copyright (C) 2013 The Trustees of Indiana University.             
#                                                                         
# Use, modification and distribution is subject to the Boost Software     
# License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at 
# http://www.boost.org/LICENSE_1_0.txt)                                   
#                                                                         
#  Authors: Jeremiah Willcock                                             
#           Andrew Lumsdaine                                              

CFLAGS_COMMON = -g -O3 -fopenmp -Wall -Wextra -Wno-unused -Drestrict=__restrict__ -DGRAPH_GENERATOR_MPI -DMPICH_SKIP_MPICXX -DOMPI_SKIP_MPICXX -I$(MPI_HOME)/include
CFLAGS = $(CFLAGS_COMMON)
CXXFLAGS = $(CFLAGS_COMMON)
LDFLAGS = -O3 -fopenmp -L$(MPI_HOME)/lib -Drestrict=__restrict__ -lmpich -lstdc++
MPICC = gcc
MPICXX = gcc
KERNELCC = gcc
KERNELCFLAGS = -O3 -std=gnu99 -g -DUSE_ASM_VERSIONS_FOR_MATVEC -Drestrict=__restrict__

SIZES = 2 8 16 32 128 512 1352 2048 2592 5408 8192 10368 32768
EXES = $(SIZES:%=graph500_mpi_custom_%)
all: $(EXES)

GENERATOR_OBJECTS = ../generator/graph_generator.o ../generator/make_graph.o ../generator/splittable_mrg.o ../generator/utils.o
OBJECTS = main.o utils.o validate.o bitmap.o kernels.o kernels_tweaked.o
HEADERS = bitmap.hpp common.hpp mpi_workarounds.hpp onesided.hpp coroutine.hpp kernels.h

$(EXES): graph500_mpi_custom_%: bfs_custom_%.o $(OBJECTS) $(GENERATOR_OBJECTS)
	$(MPICXX) -o $@ $(OBJECTS) $(GENERATOR_OBJECTS) $< -lm $(LDFLAGS)

bfs_custom_%.o: bfs_custom.cpp $(HEADERS)
	$(MPICXX) $(CXXFLAGS) -DPROCESS_COUNT=$(@:bfs_custom_%.o=%) -c bfs_custom.cpp -o $@

$(OBJECTS) : $(HEADERS)

.s.o:
	$(MPICC) $(CFLAGS) -c $< -o $@

.c.o: $(HEADERS)
	$(MPICC) $(CFLAGS) -c $< -o $@

.cpp.o: $(HEADERS)
	$(MPICXX) $(CXXFLAGS) -c $< -o $@

kernels.o: kernels.c kernels.h
	$(KERNELCC) $(KERNELCFLAGS) -c $<

clean:
	-rm -f graph500_mpi_custom_* *.o *.a ../generator/*.o
