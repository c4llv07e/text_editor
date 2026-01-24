#!/bin/sh
set -xeu
DEBUG_ARGS="-lasan -ggdb -fsanitize=pointer-compare \
    -fsanitize=pointer-subtract -fanalyzer -fsanitize=address \
    -fsanitize=undefined -fsanitize=leak -DDEBUG=ON"
ARGS="-Wall -Wextra -pedantic -lSDL3"
ARGS="${DEBUG_ARGS} ${ARGS}"
gcc ${ARGS} -o editor editor.c
