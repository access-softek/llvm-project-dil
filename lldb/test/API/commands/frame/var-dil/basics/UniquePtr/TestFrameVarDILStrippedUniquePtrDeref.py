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

class TestFrameVarDILUniquePtrDeref(TestBase):
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

        # Test member-of dereference.
        self.expect("frame variable 'ptr_node->value'", substrs=["1"])
        self.expect("frame variable 'ptr_node->next->value'",
                    substrs=["2"])

        # Test ptr dereference.
        self.expect("frame variable '(*ptr_node).value'", substrs=["1"])
        self.expect("frame variable '(*(*ptr_node).next).value'",
                    substrs=["2"])
