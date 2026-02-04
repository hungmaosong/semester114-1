.PHONY: all clean

CC = gcc
DPU_CC = dpu-upmem-dpurte-clang

EXECUTABLE = main
DPU_PROGRAM = dpu_program

HOST_SRC = main.c dpu_host.c
DPU_SRC = dpu_program.c

all:
	$(CC) --std=c99 $(HOST_SRC) -o $(EXECUTABLE) `dpu-pkg-config --cflags --libs dpu`
	$(DPU_CC) $(DPU_SRC) -o $(DPU_PROGRAM)

clean:
	rm -f $(EXECUTABLE) $(DPU_PROGRAM)
