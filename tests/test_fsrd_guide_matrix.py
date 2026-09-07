"""Authenticated native ray-matrix recipe, executed under ordinary and hostile FP flags."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "OptiScaler/upscalers/ffx/FSRDCyberpunkGuideMatrix.h"


class GuideMatrix(unittest.TestCase):
    def test_recipe_is_pure_and_locally_precise(self):
        source = HEADER.read_text()
        for forbidden in ("ReadProcessMemory", "Config::", "State::", "ID3D12", "NGX_", "XMMatrix",
                          "XMVector", "std::mutex", "thread_local", "CreateThread", "std::fma(",
                          "_mm_setcsr(", "fesetround("):
            self.assertNotIn(forbidden, source)
        self.assertIn("float_control(precise, on, push)", source)
        self.assertIn('optimize("no-fast-math", "fp-contract=off")', source)
        self.assertIn("Detail::Add(products[1], products[0], sum)", source)
        self.assertIn("Detail::Add(sum, products[2], sum)", source)
        self.assertIn("Detail::Add(sum, products[3], sum)", source)
        self.assertIn("rotation[12] = rotation[13] = rotation[14] = 0", source)
        self.assertIn("WideResultInRange", source)

    def test_compiled_live_words_rounding_order_and_failures(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to a host C++20 compiler for guide matrix tests")
        harness = r'''
#include <cassert>
#include <cfenv>
#include <cstdint>
#include <cstring>
#include "FSRDCyberpunkGuideMatrix.h"
namespace M=FSRD::CyberpunkGuideMatrix;
namespace C=FSRD::CyberpunkGuideConstants;
M::MatrixWords identity()
{
    M::MatrixWords value{};
    for(unsigned i=0;i<4;++i) value[i*4+i]=0x3f800000u;
    return value;
}
uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }
int main()
{
    std::fesetround(FE_TONEAREST);
    // Real current-view sources and independently captured 20 GPU words from
    // fog-20260907-050934-789Z-324. Only numeric metadata, no proprietary shader.
    const M::MatrixWords nativeP = {
        1062804112u,2147483648u,0u,2147483648u,
        2147483648u,1056194017u,2147483648u,0u,
        0u,2147483648u,0u,3259498481u,
        3118368775u,3112188438u,1065353216u,1112014848u};
    const M::MatrixWords nativeV = {
        1052758604u,3211614910u,949814428u,0u,
        980611116u,971942931u,1065353207u,0u,
        1064131254u,1052758594u,3129408008u,0u,
        3305842408u,3308206073u,1115774745u,1065353216u};
    const C::ObservedSharedWords captured = {{
        {1050848554u,3209251468u,948250931u,0u},
        {971491235u,962835891u,1056194008u,0u},
        {0u,0u,0u,3259498481u},
        {1064128582u,1052771795u,3131509149u,1112014848u},
        {1151336448u,1144258560u,978111693u,985008993u}}};
    C::ObservedSharedWords output{};
    for(auto& row:output) row.fill(0xfedcba98u);
    const auto sentinel=output;
    assert(M::Generate(nativeP,nativeV,1280,720,output) && output==captured);
    auto reject=[&](const M::MatrixWords& p,const M::MatrixWords& v,uint32_t w=1280,uint32_t h=720)
    {
        output=sentinel;
        assert(!M::Generate(p,v,w,h,output) && output==sentinel);
    };
    for(uint32_t invalid : {0x7f800000u,0xff800000u,0x7fc12345u,0x7f800001u,
                            0x00000001u,0x007fffffu,0x80000001u,0x807fffffu})
        for(unsigned at=0;at<16;++at)
        {
            auto p=nativeP, v=nativeV;
            p[at]=invalid; reject(p,v);
            p=nativeP; v[at]=invalid; reject(p,v);
        }
    reject(nativeP,nativeV,0,720); reject(nativeP,nativeV,1280,0);
    assert(M::Generate(nativeP,nativeV,0xffffffffu,1,output));
    assert((output[4]==std::array<uint32_t,4>{0x4f800000u,0x3f800000u,0x2f800000u,0x3f800000u}));
    // Every source fourth-row component is replaced, including W.
    auto changed=nativeV;
    changed[12]=0x7f7fffffu; changed[13]=0xff7fffffu; changed[14]=0x80000000u; changed[15]=0;
    assert(M::Generate(nativeP,changed,1280,720,output) && output==captured);
    auto p=identity(), v=identity();
    p[0]=0x3f800001u; p[1]=0xbf800000u; // (1+2^-23)*(1-2^-23), rounded BEFORE adding -1.
    v[0]=0x3f7ffffeu; v[4]=0x3f800000u;
    assert(M::Generate(p,v,1280,720,output) && output[0][0]==0);
    p=identity(); v=identity();
    p[0]=bits(16777216.0f); p[1]=bits(1.0f); p[2]=bits(-16777216.0f);
    v[0]=v[4]=v[8]=bits(1.0f);
    assert(M::Generate(p,v,1280,720,output) && output[0][0]==0); // Reassociation gives1.
    p=identity(); v=identity();
    for(unsigned i=0;i<4;++i) p[i]=0x80000000u;
    assert(M::Generate(p,v,1280,720,output));
    for(uint32_t word:output[0]) assert(word==0x80000000u); // Preserve authored signed zero.
    // Late intermediate failures cannot leak earlier successfully generated rows.
    p=identity(); v=identity(); p[12]=0x7f7fffffu; v[0]=bits(2.0f); reject(p,v);
    p=identity(); v=identity(); p[12]=p[13]=0x7f7fffffu; v[4]=bits(1.0f); reject(p,v);
    // Current non-nearest mode is refused, never changed by the helper.
    for(int mode : {FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO})
    {
        assert(std::fesetround(mode)==0); reject(nativeP,nativeV);
        assert(std::fegetround()==mode);
    }
    std::fesetround(FE_TONEAREST);
#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
    const unsigned originalCsr=_mm_getcsr();
    _mm_setcsr((originalCsr&~0x6000u)|0x2000u); // SSE-only non-nearest; x87 remains nearest.
    reject(nativeP,nativeV); assert((_mm_getcsr()&0x6000u)==0x2000u);
    for(unsigned flush : {0u,0x8040u})
    {
        _mm_setcsr((originalCsr&~0xe040u)|flush);
        const unsigned controlBefore=_mm_getcsr()&~0x3fu; // Arithmetic may set exception-status flags.
        assert(M::Generate(nativeP,nativeV,1280,720,output) && output==captured);
        assert((_mm_getcsr()&~0x3fu)==controlBefore);
        p=identity(); v=identity(); p[0]=0x00800000u; v[0]=bits(0.5f); reject(p,v);
        p=identity(); v=identity(); p[0]=0x00800000u; p[1]=0x80800001u; v[4]=bits(1.0f); reject(p,v);
        // Product too tiny even for subnormal binary32 must not be silently accepted as zero.
        p=identity(); v=identity(); p[0]=0x00800000u; v[0]=0x00800000u; reject(p,v);
        assert((_mm_getcsr()&~0x3fu)==controlBefore);
    }
    _mm_setcsr(originalCsr);
#endif
    assert(M::Generate(nativeP,nativeV,1280,720,output) && output==captured);
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-guide-matrix-") as name:
            directory = Path(name)
            source = directory / "matrix_test.cpp"
            source.write_text(harness)
            for optimization in ("-O0", "-O3 -ffast-math -ffp-contract=fast"):
                with self.subTest(optimization=optimization):
                    executable = directory / "matrix_test"
                    compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra",
                                               *optimization.split(), str(source), "-I", str(HEADER.parent),
                                               "-o", str(executable)], capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
