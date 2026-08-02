CC = gcc
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra
LDFLAGS =

TARGET = dailution
SRC = dailution.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -O2 -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

release: $(SRC)
	$(CC) $(CFLAGS) -O2 -s -o $(TARGET) $^ $(LDFLAGS)
