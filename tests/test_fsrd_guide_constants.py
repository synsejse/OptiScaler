"""Exact authored guide constant packing, with actual C++ under normal and fast FP flags."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkGuideConstants.h"


class GuideConstants(unittest.TestCase):
    def test_packer_does_not_obtain_or_invent_current_engine_inputs(self):
        source = HEADER.read_text()
        for forbidden in ("ReadProcessMemory", "Config::", "State::", "ID3D12", "NGX_",
                          "XMMatrix", "XMVector", "std::mutex", "thread_local", "CreateThread"):
            self.assertNotIn(forbidden, source)
        self.assertIn("TransparencyInput::Unspecified", source)
        self.assertIn("not identical to late DLSSD guides", source)
        self.assertIn("float_control(precise, on, push)", source)
        self.assertIn('optimize("no-fast-math")', source)

    def test_compiled_raw_layout_and_precise_pass(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to a host C++20 compiler for guide constant tests")
        harness = r'''
#include <cassert>
#include <cfenv>
#include <cstring>
#include "FSRDCyberpunkGuideConstants.h"
namespace C=FSRD::CyberpunkGuideConstants;
int main()
{
    std::fesetround(FE_TONEAREST);
    C::ObservedSharedWords source{};
    for(unsigned row=0;row<5;++row)
        for(unsigned col=0;col<4;++col) source[row][col]=0x3f000000u+row*4+col;
    source[0][0]=0x80000000u; // Signed zero is a native word, not arithmetic here.
    source[1][2]=0x7fc12345u; // Packing does not validate/rewrite even NaN payloads; caller owns input validity.
    const auto shared=C::PackShared(source);
    unsigned count=0;
    for(unsigned word=0;word<shared.size();++word)
    {
        bool used=false;
        for(unsigned i=0;i<5;++i)
            if(word/4==C::SharedRegisters[i])
            {
                assert(shared[word]==source[i][word%4]); ++count; used=true;
            }
        if(!used) assert(shared[word]==0);
    }
    assert(count==20);
    C::PassSources pass;
    C::PassConstants packed; packed.fill(0xfefefefeu);
    const auto sentinel=packed;
    assert(!C::PackPass(pass,packed) && packed==sentinel);
    pass.width=2560; pass.height=1440;
    assert(!C::PackPass(pass,packed) && packed==sentinel); // Transparency branch must be explicit.
    pass.transparency=C::TransparencyInput::PreTransparencySurface;
    pass.noVMode=-1; pass.extraSpecularEnabled=255; pass.extraSpecularScaleBits=0x80000000u;
    assert(C::PackPass(pass,packed));
    // Exact IEEE binary32 results of authored CVTDQ2PS and DIVSS with nearest rounding.
    assert(packed==C::PassConstants({0x45200000u,0x44b40000u,0x39cccccd,0x3a360b61,
                                    0,0xffffffffu,255,0x80000000u}));
    pass.transparency=C::TransparencyInput::AuthoredGuide;
    pass.noVMode=7; pass.extraSpecularScaleBits=0x3f800000u;
    assert(C::PackPass(pass,packed) && packed[4]==1 && packed[5]==7 && packed[7]==0x3f800000u);
    for(uint32_t invalid : {0u,65536u,0xffffffffu})
    {
        pass.width=invalid; const auto previous=packed;
        assert(!C::PackPass(pass,packed) && packed==previous);
    }
    pass.width=65535; pass.height=1;
    assert(C::PackPass(pass,packed));
    assert(packed[0]==0x477fff00u && packed[2]==0x37800080u && packed[3]==0x3f800000u);
    for(uint32_t invalid : {0x7f800000u,0xff800000u,0x7fc00000u})
    {
        pass.extraSpecularScaleBits=invalid; const auto previous=packed;
        assert(!C::PackPass(pass,packed) && packed==previous);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-guide-constants-") as name:
            directory = Path(name)
            source = directory / "constants_test.cpp"
            source.write_text(harness)
            for optimization in ("-O0", "-O3 -ffast-math"):
                with self.subTest(optimization=optimization):
                    executable = directory / "constants_test"
                    compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra",
                                               *optimization.split(), str(source), "-I", str(HEADER.parent),
                                               "-o", str(executable)], capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
