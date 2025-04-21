"""
Make sure 'frame var' using DIL parser/evaultor works for local variables.
"""

import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
from lldbsuite.test import lldbutil

import os
import shutil
import time

class TestFrameVarDILUniquePtr(TestBase):
    # If your test case doesn't stress debug info, then
    # set this to true.  That way it won't be run once for
    # each debug info format.
    NO_DEBUG_INFO_TESTCASE = True

    def test_frame_var(self):
        self.build()
        lldbutil.run_to_source_breakpoint(self, "Set a breakpoint here",
                                          lldb.SBFileSpec("main.cpp"))

        self.expect("settings set target.experimental.use-DIL true",
                    substrs=[""])

        self.expect("frame variable 'ptr_int == nullptr'", substrs=["false"])
        self.expect("frame variable 'ptr_int != nullptr'", substrs=["true"])
        self.expect("frame variable 'ptr_int == ptr_int'", substrs=["true"])

        # C++ doesn't allow comparing unique_ptr with raw pointers, but we
        # allow it for convenience.
        self.expect("frame variable 'ptr_int == 0'", substrs=["false"])
        self.expect("frame variable 'ptr_int == (int*)0'", substrs=["false"])

        self.expect("frame variable 'ptr_float == nullptr'", substrs=["false"])
        self.expect("frame variable 'ptr_float != nullptr'", substrs=["true"])
        self.expect("frame variable 'ptr_float == (float*)0'", substrs=["false"])

        self.expect("frame variable 'ptr_float == (int*)0'", error=True,
                    substrs=["comparison of distinct pointer types"])
        self.expect("frame variable 'ptr_int == ptr_float'", error=True,
                    substrs=["comparison of distinct pointer types"])

        self.expect("frame variable 'ptr_null == nullptr'", substrs=["true"])
        self.expect("frame variable 'ptr_null != nullptr'", substrs=["false"])

        self.expect("frame variable 'ptr_void == nullptr'", substrs=["false"])
        self.expect("frame variable 'ptr_void != nullptr'", substrs=["true"])
        self.expect("frame variable 'ptr_void == ptr_void'", substrs=["true"])

        # Void pointer can be compared with everything.
        self.expect("frame variable 'ptr_void == (int*)0'", substrs=["false"])
        self.expect("frame variable 'ptr_void == (void*)0'", substrs=["false"])
        self.expect("frame variable 'ptr_int == ptr_void'", substrs=["false"])
        self.expect("frame variable 'ptr_float == ptr_void'", substrs=["false"])
