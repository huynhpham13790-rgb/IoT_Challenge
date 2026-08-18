# Define minimal required version of CMake.
cmake_minimum_required(VERSION "3.25")

# Project definition
project(
	Xg26_Gateway_NCP
	VERSION 1.0
	LANGUAGES C CXX ASM
)

# Include the definition of the slc_Xg26_Gateway_NCP target,
# which contains the content of the SLC project
include(Xg26_Gateway_NCP.cmake)

add_executable(Xg26_Gateway_NCP
    # Add additional sources here
)

target_include_directories(Xg26_Gateway_NCP PUBLIC
    # Add additional include paths here
)

target_compile_definitions(Xg26_Gateway_NCP PUBLIC
    # Add additional macros here
)

target_compile_options(Xg26_Gateway_NCP PUBLIC
    # Set additional compiler flags here
)

target_link_options(Xg26_Gateway_NCP PUBLIC
    # Set additional linker flags here
)

# Link with the content defined in the SLC project
target_link_libraries(Xg26_Gateway_NCP PRIVATE
    slc
)

# Include managed project content if available
include(Xg26_Gateway_NCP_project.cmake OPTIONAL RESULT_VARIABLE managed_project)
if(managed_project)
    message(STATUS "Using managed project content from ${managed_project}")
endif()

# Force the gcc linker command
set_target_properties(Xg26_Gateway_NCP PROPERTIES LINKER_LANGUAGE C)

# Create .bin, .hex and .s37 artifacts after building the project
add_custom_command(TARGET Xg26_Gateway_NCP
    POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_SREC_CMD} "$<TARGET_FILE:Xg26_Gateway_NCP>" "$<TARGET_FILE_DIR:Xg26_Gateway_NCP>/$<TARGET_FILE_BASE_NAME:Xg26_Gateway_NCP>.s37"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_IHEX_CMD} "$<TARGET_FILE:Xg26_Gateway_NCP>" "$<TARGET_FILE_DIR:Xg26_Gateway_NCP>/$<TARGET_FILE_BASE_NAME:Xg26_Gateway_NCP>.hex"
    COMMAND ${CMAKE_OBJCOPY} ${OBJCOPY_BIN_CMD}  "$<TARGET_FILE:Xg26_Gateway_NCP>" "$<TARGET_FILE_DIR:Xg26_Gateway_NCP>/$<TARGET_FILE_BASE_NAME:Xg26_Gateway_NCP>.bin" 
)

# Run post-build pipeline to perform additional post-processing
if(post_build_command)
add_custom_command(TARGET Xg26_Gateway_NCP
    POST_BUILD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
    COMMAND ${post_build_command}
)
endif()