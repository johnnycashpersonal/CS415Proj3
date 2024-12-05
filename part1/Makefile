CC = gcc
CFLAGS = -Wall -pthread
DEPS = account.h string_parser.h
OBJ = bank.o string_parser.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

bank: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)
	mkdir -p output

clean:
	rm -f *.o bank output.txt
	rm -rf output/