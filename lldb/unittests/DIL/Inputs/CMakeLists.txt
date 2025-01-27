# Build `test_binary.cc` and put the binary in the Inputs folder, 
# allowing `lldb_private::GetInputFilePath` to find it.
get_target_property(EXE_PATH LLDBDILTests RUNTIME_OUTPUT_DIRECTORY)
add_custom_command(
    OUTPUT test_binary.bin
    COMMAND $<TARGET_FILE:clang> test_binary.cc -std=c++17 -fuse-ld=lld -B$<TARGET_FILE:lld> 
            -O0 -g -stdlib=libc++ -lc++ -o ${EXE_PATH}/Inputs/test_binary.bin
    DEPENDS test_binary.cc clang lld
)
add_custom_target(test_binary
    DEPENDS test_binary.bin
)
