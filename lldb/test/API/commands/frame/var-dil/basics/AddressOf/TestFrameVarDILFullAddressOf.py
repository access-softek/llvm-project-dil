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

class TestFrameVarDILAddressOf(TestBase):
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
        self.expect("frame variable '&x'", patterns=["0x[0-9]+"])
        self.expect("frame variable 'r'", substrs=["42"])
        self.expect("frame variable '&r'", patterns=["0x[0-9]+"])
        self.expect("frame variable 'pr'", patterns=["0x[0-9]+"])
        self.expect("frame variable '&pr'", patterns=["0x[0-9]+"])
        self.expect("frame variable 'my_pr'", patterns=["0x[0-9]+"])
        self.expect("frame variable '&my_pr'", patterns=["0x[0-9]+"])

        self.expect("frame variable '&x == &r'", substrs=["true"])
        self.expect("frame variable '&x != &r'", substrs=["false"])

        self.expect("frame variable '&p == &pr'", substrs=["true"])
        self.expect("frame variable '&p != &pr'", substrs=["false"])
        self.expect("frame variable '&p == &my_pr'", substrs=["true"])
        self.expect("frame variable '&p != &my_pr'", substrs=["false"])

        self.expect("frame variable '&globalVar'", patterns=["0x[0-9]+"])
        self.expect("frame variable '&s_str'", patterns=["0x[0-9]+"])
        self.expect("frame variable '&param'", patterns=["0x[0-9]+"])

        self.expect("frame variable '&(true ? x : x)'",
                    patterns=["0x[0-9]+"])
        self.expect("frame variable '&(true ? c : c)'",
                    patterns=["0x[0-9]+"])

        self.expect("frame variable '&externGlobalVar'", error=True,
                    substrs=["use of undeclared identifier 'externGlobalVar'"])

        self.expect("frame variable '&1'", error=True,
                    substrs=["cannot take the address of an rvalue of type "
                             "'int'"])

        self.expect("frame variable '&0.1'", error=True,
                    substrs=["cannot take the address of an rvalue of type "
                             "'double'"])

        self.expect("frame variable '&(true ? 1 : 1)'", error=True,
                    substrs=["cannot take the address of an rvalue of type "
                             "'int'"])

        self.expect("frame variable '&(true ? c : (char)1)'", error=True,
                    substrs=["cannot take the address of an rvalue of type "
                             "'char'"])
        self.expect("frame variable '&(true ? c : 1)'", error=True,
                    substrs=["cannot take the address of an rvalue of type "
                             "'int'"])

        self.expect("frame variable '&(&s_str)'", error=True,
                    substrs=["cannot take the address of an rvalue of type "
                             "'const char **'"])
