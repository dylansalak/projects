##################################################
# Makefile template to build the define target
##################################################

# C compiler
CC = gcc
# C compiler flags
CFLAGS = -g -Wall -Wextra -Wpedantic -Wshadow -O2 -pthread
# Target executable name
TARGET = httpserver

all: $(TARGET)

# Used to build executable from .o file(s)
$(TARGET): $(TARGET).o
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).o -lm

# Used to build .o file(s) from .c files(s)
$(TARGET).o: $(TARGET).c
	$(CC) $(CFLAGS) -c $(TARGET).c

# clean build artifacts
clean:
	rm -f $(TARGET).o

# clean built artifacts and remove executable
spotless:
	rm -f $(TARGET) $(TARGET).o

