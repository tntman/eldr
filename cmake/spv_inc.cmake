# cmake -DSPV=<in.spv> -DINC=<out.inc> -P spv_inc.cmake: a SPIR-V binary as
# the C word list glslc -mfmt=c writes ({0x07230203,...}), for #include.
# With -DOPT=<spirv-opt> -DIN=<unoptimized.spv> it first optimizes IN into
# SPV: spirv-opt -O with a high id bound, the ids compacted after. On the
# largest ray tracing stage spirv-opt sometimes dies without a word (an
# access violation, another place each run), so a failed run is named with
# its exit code and tried again, up to three times.
if(OPT)
    foreach(attempt 1 2 3)
        execute_process(COMMAND ${OPT} -O --max-id-bound=268435455 --strip-debug --compact-ids ${IN} -o ${SPV}
                        RESULT_VARIABLE code ERROR_VARIABLE errors)
        if(code EQUAL 0)
            break()
        endif()
        message(STATUS "spirv-opt failed on ${IN} (attempt ${attempt} of 3): exit code ${code} ${errors}")
    endforeach()
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "spirv-opt failed three times on ${IN}: exit code ${code} "
                            "(-1073741819 or 3221225477: it crashed; it needs about 9 GB of memory)")
    endif()
endif()
file(READ ${SPV} hex HEX)
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1,\n" words "${hex}")
file(WRITE ${INC} "{${words}}\n")
