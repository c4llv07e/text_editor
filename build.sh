#!/bin/sh
set -eu
DEBUG_ARGS="-ggdb -DDEBUG=ON"
SUPER_DEBUG_ARGS="-lasan -ggdb -fsanitize=pointer-compare \
    -fsanitize=pointer-subtract -fanalyzer -fsanitize=address \
    -fsanitize=undefined -fsanitize=leak -DDEBUG=ON"
# DEBUG_ARGS="${DEBUG_ARGS} ${SUPER_DEBUG_ARGS}"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_LAYOUT=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_SORT=ON"
DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_SCROLL=ON"
DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_FILES=ON"
ARGS="-Wall -Wextra -pedantic -lSDL3 -Wno-missing-braces"
ARGS="${DEBUG_ARGS} ${ARGS}"
gcc -o editor editor.c ${ARGS} "${@}"
