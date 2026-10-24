# SPDX-License-Identifier: BSD-3-Clause
# Turns an urbiscript file into a C header holding its serialized chunk.
#
#   cmake -DURBI=<host urbi binary> -DIN=<file.u> -DOUT=<header> -DSYM=<name> -P bake.cmake
#
# The host `urbi --dump-wire-format` writes the chunk the library loads
# with urbi_load; the format does not depend on the target, which is how
# the standard library's own blob is baked for every cross build.
if (NOT URBI OR NOT IN OR NOT OUT OR NOT SYM)
    message(FATAL_ERROR "bake.cmake needs -DURBI= -DIN= -DOUT= -DSYM=")
endif ()
set(RAW "${OUT}.uc")
execute_process(
    COMMAND "${URBI}" --dump-wire-format "${IN}"
    OUTPUT_FILE "${RAW}"
    RESULT_VARIABLE rc
    ERROR_VARIABLE err)
if (NOT rc EQUAL 0)
    message(FATAL_ERROR "baking ${IN} failed (${rc}): ${err}")
endif ()
file(READ "${RAW}" HEX_CONTENT HEX)
string(LENGTH "${HEX_CONTENT}" HEX_LEN)
math(EXPR BYTE_LEN "${HEX_LEN} / 2")
if (BYTE_LEN EQUAL 0)
    message(FATAL_ERROR "baking ${IN} produced no bytes")
endif ()
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BODY "${HEX_CONTENT}")
file(WRITE "${OUT}"
    "/* Generated from ${IN} by bake.cmake; do not edit. */\n"
    "#include <stddef.h>\n"
    "static const unsigned char ${SYM}[] = {\n${BODY}\n};\n"
    "static const size_t ${SYM}_len = ${BYTE_LEN}u;\n")
