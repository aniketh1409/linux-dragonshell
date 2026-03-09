CC = gcc
CFLAGS = -Wall -g
TARGET = dragonshell
OBJECTS = dragonshell.o

all: $(TARGET)

dragonshell: $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS)

compile: $(OBJECTS)

dragonshell.o: dragonshell.c
	$(CC) $(CFLAGS) -c dragonshell.c

clean:
	rm -f $(TARGET) $(OBJECTS)
