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
        self.do_test()

    def do_test(self):
        target = self.createTestTarget()

        # Now create a breakpoint in main.c at the source matching
        # "Set a breakpoint here"
        breakpoint = target.BreakpointCreateBySourceRegex(
            "Set a breakpoint here", lldb.SBFileSpec("main.cpp")
        )
        self.assertTrue(
            breakpoint and breakpoint.GetNumLocations() >= 1, VALID_BREAKPOINT
        )

        error = lldb.SBError()
        # This is the launch info.  If you want to launch with arguments or
        # environment variables, add them using SetArguments or
        # SetEnvironmentEntries

        launch_info = target.GetLaunchInfo()
        process = target.Launch(launch_info, error)
        self.assertTrue(process, PROCESS_IS_VALID)

        # Did we hit our breakpoint?
        from lldbsuite.test.lldbutil import get_threads_stopped_at_breakpoint

        threads = get_threads_stopped_at_breakpoint(process, breakpoint)
        self.assertEqual(
            len(threads), 1, "There should be a thread stopped at our breakpoint"
        )
       # The hit count for the breakpoint should be 1.
        self.assertEquals(breakpoint.GetHitCount(), 1)

        frame = threads[0].GetFrameAtIndex(0)
        command_result = lldb.SBCommandReturnObject()
        interp = self.dbg.GetCommandInterpreter()

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
