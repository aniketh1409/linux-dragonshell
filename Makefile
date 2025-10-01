CC = gcc
CFLAGS = -Wall -g
TARGET = dragonshell

all: $(TARGET)

$(TARGET): dragonshell.c
	$(CC) $(CFLAGS) -o $(TARGET) dragonshell.c

clean:
	rm -f $(TARGET) *.o
