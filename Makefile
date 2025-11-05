CC = gcc
CFLAGS = -Wall -Wextra -I../../includes -I. -std=c11
LIBS = -lglfw -lGLEW -lGL -lm

# Source files
SRCS = terminal.c window_manager_working.c font-render.c ansi.c
OBJS = $(SRCS:.c=.o)

# Output binary
TARGET = terminal

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

terminal.o: terminal.c window_manager.h font-render.h types.h
	$(CC) $(CFLAGS) -c terminal.c

window_manager_working.o: window_manager_working.c window_manager.h font-render.h types.h
	$(CC) $(CFLAGS) -c window_manager_working.c

font-render.o: font-render.c font-render.h
	$(CC) $(CFLAGS) -c font-render.c

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
