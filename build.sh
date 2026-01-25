#!/bin/sh
set -xeu
DEBUG_ARGS="-lasan -ggdb -fsanitize=pointer-compare \
    -fsanitize=pointer-subtract -fanalyzer -fsanitize=address \
    -fsanitize=undefined -fsanitize=leak -DDEBUG=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_LAYOUT=ON"
ARGS="-Wall -Wextra -pedantic -lSDL3 -Wno-missing-braces"
ARGS="${DEBUG_ARGS} ${ARGS}"
gcc ${ARGS} -o editor editor.c
