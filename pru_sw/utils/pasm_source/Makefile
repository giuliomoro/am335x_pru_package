$(shell mkdir -p build)
SRCS:=pasm.c pasmpp.c pasmexp.c pasmop.c pasmdot.c pasmstruct.c pasmmacro.c path_utils.c
HEADERS:=$(shell find . -name "*.h")
OBJS:=$(addprefix build/,$(SRCS:.c=.o))

build/%.o: %.c $(HEADERS)
	gcc -Wall -D_UNIX_ "$<" -c -o "$@" -g

../pasm: $(OBJS)
	gcc -o ../pasm $^

pasm.mac: $(OBJS)
	gcc -o ../pasm.mac $^

clean:
	rm -rf build ../pasm

.DEFAULT_GOAL: pasm
