set(ENABLED_SANITIZER FALSE)
if(${USE_ASAN} OR ${USE_MSAN} OR ${USE_LSAN} OR ${USE_UBSAN})
    set(ENABLED_SANITIZER TRUE)
endif()

if(MSVC)
    # gersemi: off
    set (c_compiler_options_common
        /EHsc /GA /Gw /Gy /GF /GR- /GS-
        $<$<CONFIG:Release>: $<IF:$<BOOL:${RUNTIME_STATIC_LINK}>,/MT,/MD>   $<IF:$<BOOL:${ENABLED_SANITIZER}>,/O1 /Zi,/O2>>
        $<$<CONFIG:Nightly>: $<IF:$<BOOL:${RUNTIME_STATIC_LINK}>,/MT,/MD>   $<IF:$<BOOL:${ENABLED_SANITIZER}>,/O1 /Zi,/O2>>
        $<$<CONFIG:Debug>:   $<IF:$<BOOL:${RUNTIME_STATIC_LINK}>,/MTd,/MDd> $<IF:$<BOOL:${ENABLED_SANITIZER}>,/Zi,/ZI>>
    )
    set (c_linker_options_common
        $<$<CONFIG:Release>: /OPT:REF,ICF /LTCG:ON  /NODEFAULTLIB:libcmtd>
        $<$<CONFIG:Nightly>: /OPT:REF,ICF /LTCG:ON  /NODEFAULTLIB:libcmtd>
        $<$<CONFIG:Debug>:   /DEBUG       /LTCG:OFF /NODEFAULTLIB:libcmt>
    )
    # gersemi: on

elseif(NOT MSVC)
    set(c_compiler_options_common
        -pipe
        -fexceptions
        -fnon-call-exceptions
        -O3 # -Os saves ~200 kb, probably not worth losing some performance for that amount...
        # Place each function and variable into its own section, allowing the linker to remove unused ones.
        #-ffunction-sections
        #-fdata-sections
        #-flto # Also needed to benefit the most from the two flags above.
        $<$<BOOL:${ENABLED_SANITIZER}>:-ggdb3>
    )

    if(${CMAKE_C_COMPILER_ID} STREQUAL GNU)
        set(c_compiler_options_common ${c_compiler_options_common} -fno-expensive-optimizations)
    endif()

    #set (c_compiler_definitions_common ${c_compiler_definitions_common}
    #)

    #set (c_linker_options_common ${c_linker_options_common}
    #)

    #if (${CMAKE_C_COMPILER_ID} STREQUAL Clang)
    #endif()
endif()
