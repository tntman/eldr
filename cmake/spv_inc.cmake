# cmake -DSPV=<in.spv> -DINC=<out.inc> -P spv_inc.cmake: a SPIR-V binary as
# the C word list glslc -mfmt=c writes ({0x07230203,...}), for #include.
file(READ ${SPV} hex HEX)
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1,\n" words "${hex}")
file(WRITE ${INC} "{${words}}\n")
