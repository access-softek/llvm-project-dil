add_lldb_unittest(LLDBDILTests
  DIL_test.cc
  runner.cpp

  LINK_LIBS
    liblldb
    lldbUtilityHelpers
    LLVMTestingSupport
  )
add_subdirectory(Inputs)
add_dependencies(LLDBDILTests test_binary)

add_unittest_inputs(LLDBDILTests "test_binary.cc")
