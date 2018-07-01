############################################################
# Create a library
############################################################


# Add an executable with the above sources
add_executable(hello_binary
    main.cpp
)

# link the new memory_library target with the hello_binary target


add_library(memory_library SHARED 
    treeNode.cpp
)


target_include_directories(memory_library
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries( hello_binary
    PRIVATE 
        memory_library
)


