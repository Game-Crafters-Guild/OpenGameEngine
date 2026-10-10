# Embeds a text file in a C++ header as a null-terminated byte array, so code that needs the file
# at runtime carries it in its own binary instead of depending on every executable and every
# packaging path to stage a loose copy.
#
# Invoked by the build as:
#   cmake -DSRC=<file> -DOUT=<header> -DSYMBOL=<identifier> -P EmbedTextFile.cmake
#
# The header defines `constexpr unsigned char SYMBOL[]`, the file's bytes followed by a terminating
# zero; include it inside the namespace the symbol belongs to. The bytes are written as hex so no
# file content can end a literal early and no literal length limit applies, and as unsigned char
# so bytes above 0x7F (UTF-8) are not narrowing conversions.

if(NOT DEFINED SRC OR NOT DEFINED OUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "EmbedTextFile: SRC, OUT and SYMBOL are required")
endif()
if(NOT EXISTS "${SRC}")
    message(FATAL_ERROR "EmbedTextFile: '${SRC}' does not exist")
endif()

file(READ "${SRC}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")

get_filename_component(_srcName "${SRC}" NAME)

# Written through copy_if_different so an unchanged file does not retrigger the compile.
file(WRITE "${OUT}.tmp"
     "// Embedded from ${_srcName} by EmbedTextFile.cmake. Edit the source file, not this one.\n"
     "#pragma once\n\n"
     "constexpr unsigned char ${SYMBOL}[] = {${_bytes}0x00};\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
