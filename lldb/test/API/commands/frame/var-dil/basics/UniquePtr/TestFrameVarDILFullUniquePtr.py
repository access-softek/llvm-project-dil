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
        self.expect("frame variable '*(NodeU**)&ptr_node.__ptr_'",
                    patterns=["0x[0-9]+"])
        self.expect("frame variable '(*(NodeU**)&ptr_node.__ptr_)->value'",
                    substrs=["1"])

        #self.expect("frame variable 'ptr_node.__ptr_.__value_'",
        #            patterns=["0x[0-9]+"])
        #self.expect("frame variable 'ptr_node.__ptr_.__value_->value'",
        #            substrs=["1"])
        #self.expect(
        #    "frame variable 'ptr_node.__ptr_.__value_->next.__ptr_.__value_->value'",
        #    substrs=["2"])
