include_directories(include)

add_library(apitest apitest.c)

add_subdirectory(advapi32)
add_subdirectory(browseui)
add_subdirectory(com)
add_subdirectory(crt)
add_subdirectory(dciman32)
add_subdirectory(gdi32)
if(NOT ARCH STREQUAL "amd64")
    add_subdirectory(kernel32)
endif()
add_subdirectory(msvcrt)
add_subdirectory(ntdll)
add_subdirectory(powrprof)
add_subdirectory(setupapi)
add_subdirectory(shell32)
add_subdirectory(psapi)
add_subdirectory(user32)
if(NOT ARCH STREQUAL "amd64")
    add_subdirectory(w32kdll)
    add_subdirectory(w32knapi)
endif()
add_subdirectory(winhttp)
add_subdirectory(wininet)
add_subdirectory(ws2_32)
