"""Current configured motion scaling, not hardcoded native defaults or NGX pixels."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'OptiScaler/upscalers/ffx'


class MotionScale(unittest.TestCase):
    def test_cpu_only_host_and_project_integration(self):
        header = (BASE / 'FSRDCyberpunkMotionScale.h').read_text()
        source = (BASE / 'FSRDCyberpunkEarlyGuides.cpp').read_text()
        for forbidden in ('AddRef(', 'GetDesc(', 'ResourceBarrier', 'GetGPUVirtualAddress', 'std::isfinite'):
            self.assertNotIn(forbidden, header)
        for required in ('property + 0x30', 'first != second', '0x7f800000u',
                         '0x3464d90', '0x3464de0', '0x300c8f0', '0x300c8e0'):
            self.assertIn(required, header)
        self.assertIn('result["motion_scale"] = MotionScaleProvenance(read, image)', source)
        self.assertIn('"defaults_used", false', source)
        self.assertIn('"gpu_binding_proven", false', source)
        for project in ('OptiScaler.vcxproj', 'OptiScaler.vcxproj.filters'):
            self.assertEqual((ROOT / 'OptiScaler' / project).read_text().count(
                'ClInclude Include="upscalers\\ffx\\FSRDCyberpunkMotionScale.h"'), 1)

    def test_actual_observer_preserves_values_and_refuses_mutation(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('CXX required')
        harness = r'''
#include "FSRDCyberpunkMotionScale.h"
#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
using namespace FSRD::CyberpunkMotionScale;
constexpr uintptr_t image=0x140000000;
struct Host {
    std::map<uintptr_t,std::vector<unsigned char>> data;
    unsigned reads=0, mutateAt=0; bool fail=false, throws=false;
    template<class T> void Put(uintptr_t a,const T& v) {
        auto& bytes=data[a]; bytes.resize(sizeof(v)); std::memcpy(bytes.data(),&v,sizeof(v));
    }
    bool Read(uintptr_t a,void* p,size_t n) {
        if(throws) throw std::runtime_error("read");
        ++reads;
        if(reads==mutateAt) Put(image+PropertyRvas[1]+0x30,uint32_t(0x40000000));
        if(fail || !data.contains(a) || data[a].size()!=n) return false;
        std::memcpy(p,data[a].data(),n); return true;
    }
};
Host Setup() {
    Host h;
    h.Put(image+SectionRva,std::array<char,5>{'D','L','S','S',0});
    for(unsigned i=0;i<2;++i) {
        h.Put(image+PropertyRvas[i],std::array<uintptr_t,3>{image+VtableRva,image+NameRvas[i],image+SectionRva});
        h.Put(image+NameRvas[i],std::array<char,11>{'M','v','e','c','S','c','a','l','e',char('X'+i),0});
        h.Put(image+PropertyRvas[i]+0x30,i ? uint32_t(0xbe800000) : uint32_t(0x3fc00000));
        h.Put(image+PropertyRvas[i]+0x3c,uint32_t(0x3f800000)); // unrelated default
    }
    return h;
}
int main() {
    const Snapshot sentinel{{0x1234,0x5678}};
    auto h=Setup(); Snapshot s;
    assert(Observe(h,image,s)); assert(s.words[0]==0x3fc00000 && s.words[1]==0xbe800000);
    assert(h.reads==14);
    for(auto bits:{0u,0x80000000u,1u,0x7f7fffffu}) {
        h=Setup(); h.Put(image+PropertyRvas[0]+0x30,bits);
        assert(Observe(h,image,s) && s.words[0]==bits);
    }
    for(unsigned problem=0;problem<10;++problem) {
        h=Setup(); s=sentinel; uintptr_t base=image;
        switch(problem) {
        case 0: base=0; break;
        case 1: base=std::numeric_limits<uintptr_t>::max()-10; break;
        case 2: h.fail=true; break;
        case 3: h.throws=true; break;
        case 4: h.mutateAt=8; break;
        case 5: h.Put(image+PropertyRvas[0]+0x30,uint32_t(0x7fc00001)); break;
        case 6: h.Put(image+PropertyRvas[1]+0x30,uint32_t(0xff800000)); break;
        case 7: h.Put(image+PropertyRvas[0],std::array<uintptr_t,3>{image+VtableRva,image+NameRvas[1],image+SectionRva}); break;
        case 8: h.Put(image+NameRvas[0],std::array<char,11>{}); break;
        case 9: h.Put(image+SectionRva,std::array<char,5>{}); break;
        }
        assert(!Observe(h,base,s) && s==sentinel);
    }
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-motion-scale-') as tmp:
            source = Path(tmp) / 'test.cpp'
            source.write_text(harness)
            for options in (['-O0'], ['-O3', '-ffast-math']):
                exe = Path(tmp) / 'test'
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *options,
                                '-I', str(BASE), str(source), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
