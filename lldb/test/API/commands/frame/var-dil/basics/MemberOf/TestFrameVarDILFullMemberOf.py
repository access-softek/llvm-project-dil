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

class TestFrameVarDILMemberOf(TestBase):
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
        self.expect("frame variable 's.x'", substrs=["1"])
        self.expect("frame variable 's.r'", substrs=["2"])
        self.expect("frame variable 's.r + 1'", substrs=["3"])
        self.expect("frame variable 'sr.x'", substrs=["1"])
        self.expect("frame variable 'sr.r'", substrs=["2"])
        self.expect("frame variable 'sr.r + 1'", substrs=["3"])
        self.expect("frame variable 'sp->x'", substrs=["1"])
        self.expect("frame variable 'sp->r'", substrs=["2"])
        self.expect("frame variable 'sp->r + 1'", substrs=["3"])
        self.expect("frame variable 'sarr->x'", substrs=["5"]);
        self.expect("frame variable 'sarr->r'", substrs=["2"])
        self.expect("frame variable 'sarr->r + 1'", substrs=["3"])
        self.expect("frame variable '(sarr + 1)->x'", substrs=["1"])

        self.expect("frame variable 'sp->4'", error=True,
                    substrs=["expected 'identifier', got: <'4' "
                             "(numeric_constant)>"])
        self.expect("frame variable 'sp->foo'", error=True,
                    substrs=["no member named 'foo' in 'Sx'"])
        self.expect("frame variable 'sp->r / (void*)0'", error=True,
                    substrs=["invalid operands to binary expression ('int' and "
                             "'void *')"])

        self.expect("frame variable 'sp.x'", error=True,
                    substrs=["member reference type 'Sx *' is a "
                             "pointer; did you mean to use '->'"])
        self.expect("frame variable 'sarr.x'", error=True,
                    #substrs=["member reference base type 'Sx[2]' is not a "
                    #         "structure or union"])
                    substrs=["no member named 'x' in 'Sx[2]'"])

        # Test for record typedefs.
        self.expect("frame variable 'sa.x'", substrs=["3"])
        self.expect("frame variable 'sa.y'", substrs=["'\\x04'"])
