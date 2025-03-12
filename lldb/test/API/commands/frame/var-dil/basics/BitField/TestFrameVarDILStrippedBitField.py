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

class TestFrameVarDILBitField(TestBase):
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
        self.expect("frame variable 'bf.a'", substrs=["1023"])
        self.expect("frame variable 'bf.b'", substrs=["9"])
        self.expect("frame variable 'bf.c'", substrs=["false"])
        self.expect("frame variable 'bf.d'", substrs=["true"])

        # Perform an operation to ensure we actually read the value.
        #self.expect("frame variable '0 + bf.a'", substrs=["1023"])
        #self.expect("frame variable '0 + bf.b'", substrs=["9"])
        #self.expect("frame variable '0 + bf.c'", substrs=["0"])
        #self.expect("frame variable '0 + bf.d'", substrs=["1"])

        self.expect("frame variable 'abf.a'", substrs=["1023"])
        self.expect("frame variable 'abf.b'", substrs=["'\\x0f'"])
        self.expect("frame variable 'abf.c'", substrs=["3"])

        # Perform an operation to ensure we actually read the value.
        #self.expect("frame variable 'abf.a + 0'", substrs=["1023"])
        #self.expect("frame variable 'abf.b + 0'", substrs=["15"])
        #self.expect("frame variable 'abf.c + 0'", substrs=["3"])

        # Address-of is not allowed for bit-fields.
        self.expect("frame variable '&bf.a'", error=True,
                    substrs=["address of bit-field requested"])
                    #substrs=["'bf.a' doesn't have a valid address"])
        #self.expect("frame variable '&(true ? bf.a : bf.a)'", error=True,
        #            substrs=["address of bit-field requested"])
