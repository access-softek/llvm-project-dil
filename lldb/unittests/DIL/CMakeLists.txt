add_lldb_unittest(LLDBDILTests
  DIL_test.cc
  runner.cpp

  LINK_LIBS
    liblldb
    lldbUtilityHelpers
    LLVMTestingSupport
  )

set(test_lib
  test_binary_libc++.bin
  test_binary_libstdc++.bin
  test_binary.cc
  )

add_unittest_inputs(LLDBDILTests "${test_lib}")


