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

class TestFrameVarDILInstanceVariables(TestBase):
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
        self.expect("frame variable 'this->field_'", substrs=["1"])
        self.expect("frame variable 'this.field_'", error=True,
                    substrs=["member reference type 'TestMethods *' is a pointer; did "
                             "you mean to use '->'?"])
                    #substrs=["\"this\" is a pointer and . was used to attempt to access \"field_\". Did you mean \"this->field_\"?"])

        self.expect("frame variable 'c.field_'", substrs=["-1"])
        self.expect("frame variable 'c_ref.field_'", substrs=["-1"])
        self.expect("frame variable 'c_ptr->field_'", substrs=["-1"])
        self.expect("frame variable 'c->field_'", error=True,
                    substrs=["member reference type 'C' is not a "
                             "pointer; did you mean to use '.'?"])
                    #substrs=["\"c\" is not a pointer and -> was used to attempt to access \"field_\". Did you mean \"c.field_\"?"])
