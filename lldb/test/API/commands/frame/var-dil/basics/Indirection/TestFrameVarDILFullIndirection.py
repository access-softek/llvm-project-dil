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

class TestFrameVarDILIndirection(TestBase):
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
        self.expect("frame variable '*p'", substrs=["1"])
        self.expect("frame variable 'p'", patterns=["0x[0-9]+"])
        self.expect("frame variable '*my_p'", substrs=["1"])
        self.expect("frame variable 'my_p'", patterns=["0x[0-9]+"])
        self.expect("frame variable '*my_pr'", substrs=["1"])
        self.expect("frame variable 'my_pr'", patterns=["0x[0-9]+"])

        self.expect("frame variable '*1'", error=True,
                    substrs=["indirection requires pointer operand ('int' invalid)"])
        self.expect("frame variable '*val'", error=True,
                    substrs=["indirection requires pointer operand ('int' invalid)"])
