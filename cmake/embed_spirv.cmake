# Converts a SPIR-V binary (SPV) into a C file (OUT) defining:
#   const unsigned char SYM_data[]; const unsigned int SYM_size;
file(READ ${SPV} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes ${hex})
string(LENGTH ${hex} hexlen)
math(EXPR size "${hexlen} / 2")
file(WRITE ${OUT} "/* Generated from SPIR-V, do not edit */\n")
file(APPEND ${OUT} "const unsigned char ${SYM}_spv_data[] = {${bytes}};\n")
file(APPEND ${OUT} "const unsigned int ${SYM}_spv_size = ${size}u;\n")
