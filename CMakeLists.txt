# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/../lvgl-9.4.0")
set(CONFIG_LV_BUILD_EXAMPLES OFF)
set(CONFIG_LV_BUILD_DEMOS OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
project(moriburnner)

set(RETRO_GO_ROOT "${CMAKE_SOURCE_DIR}/../retro-go-master")
set(RETRO_GO_APPS launcher retro-core gwenesis prboom-go fmsx)
set(RETRO_GO_PARTITIONS launcher retro-core gwenesis prboom-go fmsx)
set(RETRO_GO_BINARIES
    "${RETRO_GO_ROOT}/launcher/build/launcher.bin"
    "${RETRO_GO_ROOT}/retro-core/build/retro-core.bin"
    "${RETRO_GO_ROOT}/gwenesis/build/gwenesis.bin"
    "${RETRO_GO_ROOT}/prboom-go/build/prboom-go.bin"
    "${RETRO_GO_ROOT}/fmsx/build/fmsx.bin")

idf_build_get_property(python PYTHON)
idf_build_get_property(idf_path IDF_PATH)

add_custom_target(check_dual_system_sizes
    COMMAND ${python} "${CMAKE_SOURCE_DIR}/tools/check_dual_system_sizes.py"
        --partition-csv "${CMAKE_SOURCE_DIR}/partitions.csv"
        --flash-args "${CMAKE_BINARY_DIR}/flash_args"
        --flash-size "16MB"
    DEPENDS app partition_table_bin
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL
    VERBATIM)

if(EXISTS "${RETRO_GO_ROOT}/rg_tool.py")
    add_custom_target(retro_go_bins
        COMMAND ${CMAKE_COMMAND} -E env
            "IDF_PATH=${idf_path}"
            "IDF_TOOLS_PATH=$ENV{IDF_TOOLS_PATH}"
            "IDF_PYTHON_ENV_PATH=$ENV{IDF_PYTHON_ENV_PATH}"
            "ESP_ROM_ELF_DIR=$ENV{ESP_ROM_ELF_DIR}"
            "PYTHON=${python}"
            ${python} rg_tool.py --target moriburnner build ${RETRO_GO_APPS}
        WORKING_DIRECTORY ${RETRO_GO_ROOT}
        USES_TERMINAL
        VERBATIM)

    if(TARGET flash)
        add_dependencies(flash retro_go_bins)
    endif()

    if(TARGET merge-bin)
        add_dependencies(merge-bin retro_go_bins)
    endif()

    add_dependencies(check_dual_system_sizes retro_go_bins)

    list(LENGTH RETRO_GO_PARTITIONS retro_go_partition_count)
    math(EXPR retro_go_partition_last "${retro_go_partition_count} - 1")
    foreach(retro_go_index RANGE ${retro_go_partition_last})
        list(GET RETRO_GO_PARTITIONS ${retro_go_index} retro_go_partition)
        list(GET RETRO_GO_BINARIES ${retro_go_index} retro_go_binary)
        if(TARGET flash)
            esptool_py_flash_to_partition(flash "${retro_go_partition}" "${retro_go_binary}")
        endif()
    endforeach()
endif()
