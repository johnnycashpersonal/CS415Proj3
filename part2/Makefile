CC = gcc
CFLAGS = -Wall -pthread -g
LDFLAGS = -rdynamic
DEPS = account.h string_parser.h
OBJ = bank.o string_parser.o

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

bank: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS)
	mkdir -p Output

clean:
	rm -f *.o bank output.txt
	rm -rf Output/