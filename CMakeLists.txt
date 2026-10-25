cmake_minimum_required(VERSION 3.11)
project(mlbb_lite_psp C)

add_executable(mlbb_lite main.c)
target_link_libraries(mlbb_lite PRIVATE pspgu pspge pspdisplay pspctrl pspkernel m)

create_pbp_file(
    TARGET mlbb_lite
    TITLE "MLBB Lite PSP"
    VERSION 01.00
)
