#!/bin/sh
set -eu
DEBUG_ARGS="-ggdb -DDEBUG=ON"
SUPER_DEBUG_ARGS="-lasan -ggdb -fsanitize=pointer-compare \
    -fsanitize=pointer-subtract -fanalyzer -fsanitize=address \
    -fsanitize=undefined -fsanitize=leak -DDEBUG=ON"
# DEBUG_ARGS="${DEBUG_ARGS} ${SUPER_DEBUG_ARGS}"
# DEBUG_ARGS="${DEBUG_ARGS} -DNO_MAIN=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_VISLINES=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_RENDER_FAN=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_LAYOUT=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_CURSOR=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_SORT=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_SCROLL=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_FILES=ON"
DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_SCROLL_ONELINE=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_BUFFERS=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_QUIT=ON"
# DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_UNDO=ON"
ADDITIONAL_FILES=""
old_pwd=${PWD}
(cd /usr/share/fonts/TTF/liberation/;
 ld -r -b binary -o "${old_pwd}/liberation_mono.o" LiberationMono-Regular.ttf)
ADDITIONAL_FILES="${ADDITIONAL_FILES} liberation_mono.o"
DEBUG_ARGS="${DEBUG_ARGS} -DDEBUG_GDB=ON"
ARGS="-Wall -Wextra -pedantic -fpic -lSDL3_ttf -lSDL3 -Wno-missing-braces"
ARGS="${DEBUG_ARGS} ${ARGS}"
ARGS="${ARGS} -DDISABLE_LOG_BUFFER=ON"
gcc -o editor editor.c ${ADDITIONAL_FILES} ${ARGS} "${@}"
