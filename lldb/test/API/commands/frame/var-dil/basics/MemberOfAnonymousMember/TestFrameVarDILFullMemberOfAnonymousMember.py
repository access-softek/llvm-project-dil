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

class TestFrameVarDILMemberOfAnonymousMember(TestBase):
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
        self.expect("frame variable 'a.x'", substrs=["1"])
        self.expect("frame variable 'a.y'", substrs=["2"])

        self.expect("frame variable 'b.x'", error=True,
                    substrs=["no member named 'x' in 'B'"])
        self.expect("frame variable 'b.y'", error=True,
                    substrs=["no member named 'y' in 'B'"])
        self.expect("frame variable 'b.z'", substrs=["3"])
        self.expect("frame variable 'b.w'", substrs=["4"])
        self.expect("frame variable 'b.a.x'", substrs=["1"])
        self.expect("frame variable 'b.a.y'", substrs=["2"])

        self.expect("frame variable 'c.x'", substrs=["5"])
        self.expect("frame variable 'c.y'", substrs=["6"])

        self.expect("frame variable 'd.x'", substrs=["7"])
        self.expect("frame variable 'd.y'", substrs=["8"])
        self.expect("frame variable 'd.z'", substrs=["9"])
        self.expect("frame variable 'd.w'", substrs=["10"])

        self.expect("frame variable 'e.x'", error=True,
                    substrs=["no member named 'x' in 'E'"])
        self.expect("frame variable 'f.x'", error=True,
                    substrs=["no member named 'x' in 'F'"])
        self.expect("frame variable 'f.named_field.x'", substrs=["12"])

        self.expect("frame variable 'unnamed_derived.x'", substrs=["1"])
        self.expect("frame variable 'unnamed_derived.y'", substrs=["2"])
        self.expect("frame variable 'unnamed_derived.z'", substrs=["13"])

        self.expect("frame variable 'derb.x'", error=True,
                    substrs=["no member named 'x' in 'DerivedB'"])
        self.expect("frame variable 'derb.y'", error=True,
                    substrs=["no member named 'y' in 'DerivedB'"])
        self.expect("frame variable 'derb.z'", substrs=["3"])
        self.expect("frame variable 'derb.w'", substrs=["14"])
        self.expect("frame variable 'derb.k'", substrs=["15"])
        self.expect("frame variable 'derb.a.x'", substrs=["1"])
        self.expect("frame variable 'derb.a.y'", substrs=["2"])
