"""
Make sure 'frame var' using DIL parser/evaultor works for C-Style casts..
"""

import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
from lldbsuite.test import lldbutil

import os
import shutil
import time

class TestFrameVarDILArithmetic(TestBase):
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
       # TestCastDerivedToBase

        self.expect("frame variable 'static_cast<CxxA*>(&a)->a'",
                    substrs=["1"])
        self.expect("frame variable 'static_cast<CxxA*>(&c)->a'",
                    substrs=["3"])
        self.expect("frame variable 'static_cast<CxxB*>(&c)->b'",
                    substrs=["4"])
        # CAROLINE!!
        #self.expect("frame variable 'static_cast<CxxB*>(&c)->c'",
        #            error=True, substrs=["no member named 'c' in 'CxxB'"])
        self.expect("frame variable 'static_cast<CxxB*>(&e)->b'",
                    substrs=["8"])
        self.expect("frame variable 'static_cast<CxxC*>(&e)->a'",
                    substrs=["7"])
        self.expect("frame variable 'static_cast<CxxC*>(&e)->b'",
                    substrs=["8"])
        self.expect("frame variable 'static_cast<CxxC*>(&e)->c'",
                    substrs=["9"])
        self.expect("frame variable 'static_cast<CxxB*>(&d)'", error=True,
                    substrs=["static_cast from 'CxxD *' to 'CxxB *', which "
                             "are not related by inheritance, is not allowed"])

        # Cast via virtual inheritance.
        self.expect("frame variable 'static_cast<CxxA*>(&vc)->a'",
                    substrs=["12"])
        self.expect("frame variable 'static_cast<CxxB*>(&vc)->b'",
                    substrs=["13"])
        # CAROLINE!!
        #self.expect("frame variable 'static_cast<CxxB*>(&vc)->c'",
        #            error=True, substrs=["no member named 'c' in 'CxxB'"])
        self.expect("frame variable 'static_cast<CxxB*>(&ve)->b'",
                    substrs=["16"])
        self.expect("frame variable 'static_cast<CxxC*>(&ve)'", error=True,
                    substrs=["static_cast from 'CxxVE *' to 'CxxC *', which "
                             "are not related by inheritance, is not allowed"])

        # Same with references.
        self.expect("frame variable 'static_cast<CxxA&>(a).a'", substrs=["1"])
        self.expect("frame variable 'static_cast<CxxA&>(c).a'", substrs=["3"])
        self.expect("frame variable 'static_cast<CxxB&>(c).b'", substrs=["4"])
        # CAROLINE!!
        #self.expect("frame variable 'static_cast<CxxB&>(c).c'",
        #            error=True, substrs=["no member named 'c' in 'CxxB'"])
        self.expect("frame variable 'static_cast<CxxB&>(e).b'", substrs=["8"])
        self.expect("frame variable 'static_cast<CxxC&>(e).a'", substrs=["7"])
        self.expect("frame variable 'static_cast<CxxC&>(e).b'", substrs=["8"])
        self.expect("frame variable 'static_cast<CxxC&>(e).c'", substrs=["9"])
        self.expect("frame variable 'static_cast<CxxB&>(d)'", error=True,
                    substrs=["static_cast from 'CxxD' to 'CxxB &', which are "
                             "not related by inheritance, is not allowed"])

        self.expect("frame variable 'static_cast<CxxA&>(vc).a'",
                    substrs=["12"])
        self.expect("frame variable 'static_cast<CxxB&>(vc).b'",
                    substrs=["13"])
        # CAROLINE!!
        #self.expect("frame variable 'static_cast<CxxB&>(vc).c'",
        #            error=True, substrs=["no member named 'c' in 'CxxB'"])
        self.expect("frame variable 'static_cast<CxxB&>(ve).b'",
                    substrs=["16"])
        self.expect("frame variable 'static_cast<CxxC&>(ve)'", error=True,
                    substrs=["static_cast from 'CxxVE' to 'CxxC &', which are"
                             " not related by inheritance, is not allowed"])


        # TestCastBaseToDerived
        # CAROLINE!!
        #self.expect("frame variable 'static_cast<CxxE*>(e_as_b)->a'",
        #            substrs=["7"])
        self.expect("frame variable 'static_cast<CxxE*>(e_as_b)->b'",
                    substrs=["8"])
        #self.expect("frame variable 'static_cast<CxxE*>(e_as_b)->c'",
        #            substrs=["9"])
        self.expect("frame variable 'static_cast<CxxE*>(e_as_b)->d'",
                    substrs=["10"])
        self.expect("frame variable 'static_cast<CxxE*>(e_as_b)->e'",
                    substrs=["11"])

        # Same with references.
        self.expect("frame variable 'static_cast<CxxE&>(*e_as_b).a'",
                    substrs=["7"])
        self.expect("frame variable 'static_cast<CxxE&>(*e_as_b).b'",
                    substrs=["8"])
        self.expect("frame variable 'static_cast<CxxE&>(*e_as_b).c'",
                    substrs=["9"])
        self.expect("frame variable 'static_cast<CxxE&>(*e_as_b).d'",
                    substrs=["10"])
        self.expect("frame variable 'static_cast<CxxE&>(*e_as_b).e'",
                    substrs=["11"])

        # Base-to-derived conversion isn't possible for virtually inherited
        # types.
        self.expect("frame variable 'static_cast<CxxVE*>(ve_as_b)'", error=True,
                    substrs=["cannot cast 'CxxB *' to 'CxxVE *' via virtual "
                             "base 'CxxB'"])
        self.expect("frame variable 'static_cast<CxxVE&>(*ve_as_b)'", error=True,
                    substrs=["cannot cast 'CxxB' to 'CxxVE &' via virtual "
                             "base 'CxxB'"])
