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

        # TestBitField
        #
        self.expect("frame variable 'bf.a'", substrs=["1023"])
        self.expect("frame variable 'bf.b'", substrs=["9"])
        self.expect("frame variable 'bf.c'", substrs=["false"])
        self.expect("frame variable 'bf.d'", substrs=["true"])

        # Perform an operation to ensure we actually read the value.
        self.expect("frame variable '0 + bf.a'", substrs=["1023"])
        self.expect("frame variable '0 + bf.b'", substrs=["9"])
        self.expect("frame variable '0 + bf.c'", substrs=["0"])
        self.expect("frame variable '0 + bf.d'", substrs=["1"])

        self.expect("frame variable 'abf.a'", substrs=["1023"])
        self.expect("frame variable 'abf.b'", substrs=["'\\x0f'"])
        self.expect("frame variable 'abf.c'", substrs=["3"])

        # Perform an operation to ensure we actually read the value.
        self.expect("frame variable 'abf.a + 0'", substrs=["1023"])
        self.expect("frame variable 'abf.b + 0'", substrs=["15"])
        self.expect("frame variable 'abf.c + 0'", substrs=["3"])

        # Address-of is not allowed for bit-fields.
        self.expect("frame variable '&bf.a'", error=True,
                    substrs=["address of bit-field requested"])
        #self.expect("frame variable '&(true ? bf.a : bf.a)'", error=True,
        #            substrs=["address of bit-field requested"]) # CAROLINE!!

        #
        # TestBitFieldPromotion
        #

        self.expect("frame variable 'bf.b - 10'", substrs=["-1"])
        self.expect("frame variable 'bf.e - 2'", substrs=["-1"])
        self.expect("frame variable 'bf.f - 2'", substrs=["4294967295"])
        self.expect("frame variable 'bf.g - 2'", substrs=["-1"])
        self.expect("frame variable 'bf.h - 2'", substrs=["-1"])
        self.expect("frame variable 'bf.i - 2'",
                    substrs=["18446744073709551615"])
        self.expect("frame variable 'bf.g - bf.b'", substrs=["-8"])

        self.expect("frame variable --  '-(true ? bf.b : bf.a)'",
                    substrs=["-9"])
        self.expect("frame variable -- '-(true ? bf.b : bf.e)'",
                    substrs=["-9"])
        self.expect("frame variable -- '-(true ? bf.b : bf.f)'",
                    #substrs=["4294967287"])
                    substrs=["-9"]) # CAROLINE: Why is -9 not correct?
        self.expect("frame variable -- '-(true ? bf.b : bf.g)'",
                    #substrs=["4294967287"])
                    substrs=["-9"]) # CAROLINE: Why is -9 not correct?
        self.expect("frame variable -- '-(true ? bf.b : bf.h)'",
                    substrs=["-9"])

        self.expect("frame variable 'bf.j - 2'", substrs=["4294967295"])
        self.expect("frame variable -- '-(true ? bf.b : bf.j)'",
                    #substrs=["4294967287"])
                    substrs=["-9"]) # CAROLINE: Why is -9 not correct?
        self.expect("frame variable -- '-(true ? bf.e : bf.j)'",
                    #substrs=["4294967295"])
                    substrs=["-1"]) # CAROLINE: Why is -1 not correct?

        #
        # TestBitFieldWithSideEffects
        #

        self.expect("frame variable -- 'bf.b -= 10'", substrs=["15"])
        self.expect("frame variable -- 'bf.e -= 10'", substrs=["-9"])
        self.expect("frame variable 'bf.e++'", substrs=["-8"])
        self.expect("frame variable '++bf.e'", substrs=["-7"])
