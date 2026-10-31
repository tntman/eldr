# cmake -DOUT=<file> -DFILES=<a;b;...> -P shader_hash.cmake: writes one
# 64-bit hash of the files' contents as a C++ literal (shader_hash.inc).
set(all "")
foreach(f ${FILES})
    file(SHA256 ${f} h)
    string(APPEND all ${h})
endforeach()
string(SHA256 h "${all}")
string(SUBSTRING ${h} 0 16 h)
file(WRITE ${OUT} "0x${h}ull\n")
