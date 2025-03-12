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

class TestFrameVarDILMemberOfInheritance(TestBase):
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
        self.expect("frame variable 'a.a_'", substrs=["1"])
        self.expect("frame variable 'b.b_'", substrs=["2"])
        self.expect("frame variable 'c.a_'", substrs=["1"])
        self.expect("frame variable 'c.b_'", substrs=["2"])
        self.expect("frame variable 'c.c_'", substrs=["3"])
        self.expect("frame variable 'd.a_'", substrs=["1"])
        self.expect("frame variable 'd.b_'", substrs=["2"])
        self.expect("frame variable 'd.c_'", substrs=["3"])
        self.expect("frame variable 'd.d_'", substrs=["4"])
        self.expect("frame variable 'd.fa_.a_'", substrs=["5"])

        #self.expect("frame variable 'bat.weight_'", substrs=["10"])

        self.expect("frame variable 'plugin.x'", substrs=["1"])
        self.expect("frame variable 'plugin.y'", substrs=["2"])

        self.expect("frame variable 'engine.x'", substrs=["1"])
        self.expect("frame variable 'engine.y'", substrs=["2"])
        self.expect("frame variable 'engine.z'", substrs=["3"])

        self.expect("frame variable 'parent_base->x'", substrs=["1"])
        self.expect("frame variable 'parent_base->y'", substrs=["2"])
        self.expect("frame variable 'parent->x'", substrs=["1"])
        self.expect("frame variable 'parent->y'", substrs=["2"])
        self.expect("frame variable 'parent->z'", substrs=["3"])
