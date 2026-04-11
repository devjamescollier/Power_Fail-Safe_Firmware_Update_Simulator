file(GLOB core_files "${ROOT}/src/core/*.c" "${ROOT}/src/core/*.h" "${ROOT}/include/fwu/*.h")

set(forbidden_patterns
    "#[ \t]*include[ \t]*<(stdio|stdlib|unistd|fcntl|malloc)\\.h>"
    "#[ \t]*include[ \t]*<sys/"
    "[^a-z_](malloc|calloc|realloc|free|fopen|fread|fwrite|open|read|write)[ \t]*\\("
)

set(violations 0)
foreach(f IN LISTS core_files)
    file(READ "${f}" content)
    string(REPLACE ";" "<semi>" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(lineno 0)
    foreach(line IN LISTS lines)
        math(EXPR lineno "${lineno} + 1")
        string(REGEX REPLACE "(->|\\.)[a-z_]+[ \t]*\\(" "(" stripped "${line}")
        foreach(pat IN LISTS forbidden_patterns)
            if(stripped MATCHES "${pat}")
                message(SEND_ERROR "${f}:${lineno}: forbidden in portable core: ${line}")
                math(EXPR violations "${violations} + 1")
            endif()
        endforeach()
    endforeach()
endforeach()

list(LENGTH core_files nfiles)
if(violations GREATER 0)
    message(FATAL_ERROR "core portability: ${violations} violation(s)")
endif()
message(STATUS "core portability: ${nfiles} files clean")
