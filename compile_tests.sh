#!/bin/bash

echo "Compiling test programs..."

# Compile all test programs
gcc -o hello hello.c
gcc -o sleeper sleeper.c  
gcc -o reader reader.c
gcc -o writer writer.c

echo "Test programs compiled successfully!"
echo "Available test programs:"
echo "  ./hello - Simple hello program with arguments"
echo "  ./sleeper - Long-running program for background testing"
echo "  ./reader - Reads from stdin (good for pipe/redirection testing)"
echo "  ./writer - Writes to stdout (good for pipe/redirection testing)"
