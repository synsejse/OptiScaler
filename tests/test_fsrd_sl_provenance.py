"""Real SL public types + actual scoped-token helper; no DLL, game or GPU calls."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "OptiScaler/hooks/StreamlineEvaluationProvenance.h"
HOOK = ROOT / "OptiScaler/hooks/Streamline_Hooks.cpp"


class StreamlineProvenance(unittest.TestCase):
    def test_scope_wraps_only_original_evaluation_and_is_opt_in(self):
        source = HOOK.read_text()
        evaluate = source.split("sl::Result StreamlineHooks::hkslEvaluateFeature(", 1)[1].split(
            "sl::Result StreamlineHooks::hkslAllocateResources(", 1)[0]
        self.assertEqual(evaluate.count("o_slEvaluateFeature("), 1)
        for gate in ("FfxDenoiserCyberpunkFogProbe", "FfxDenoiserCyberpunkFogCapture"):
            self.assertLess(evaluate.index(gate), evaluate.index("::Scope tokenScope"))
        self.assertLess(evaluate.index("::Scope tokenScope"), evaluate.index("o_slEvaluateFeature("))
        # Existing FG-none installation remains outside the subsequent FG-only tag/constants block.
        install = source.split('LOG_TRACE("Hooking v2")', 1)[1].split("auto detourResult", 1)[0]
        self.assertLess(install.index("DetourAttach(&(PVOID&) o_slEvaluateFeature"),
                        install.index("State::Instance().activeFgInput"))

    def test_no_latest_global_pointer_or_render_state(self):
        source = HELPER.read_text()
        for forbidden in ("State::", "Config::", "ID3D12", "ComPtr", "std::vector",
                          "std::unordered_map", "std::mutex", "new ", "LOG_", "slSetConstants"):
            self.assertNotIn(forbidden, source)
        self.assertIn("inline thread_local Snapshot current", source)
        self.assertIn("inline Snapshot Current() noexcept", source)
        self.assertIn("Detail::current = _previous", source)
        self.assertIn("catch (...)", source)

    def test_compiled_real_public_types_scope_and_bounds(self):
        compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("Set CXX to a host C++20 compiler for scoped token tests")
        harness = r'''
#include <cassert>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include "StreamlineEvaluationProvenance.h"
namespace P=FSRD::SlEvaluationProvenance;
struct Token : sl::FrameToken
{
    uint32_t index;
    mutable unsigned reads=0;
    bool fail=false;
    explicit Token(uint32_t value) : index(value) {}
    operator uint32_t() const override
    {
        ++reads;
        if(fail) throw std::runtime_error("mock virtual token read failure");
        return index;
    }
};
static_assert(!std::is_copy_constructible_v<P::Scope> && !std::is_move_constructible_v<P::Scope>);
static_assert(std::is_trivially_copyable_v<P::Snapshot>);

void Depth(unsigned n, Token& token)
{
    P::Scope scope(true,sl::kFeatureDLSS_RR,token,nullptr,0,nullptr);
    assert(P::Current().depth==std::min(n,P::MaxScopeDepth));
    assert(P::Current().observed==(n<=P::MaxScopeDepth));
    if(n<P::MaxScopeDepth+3) Depth(n+1,token);
}

int main()
{
    Token outerToken(700),innerToken(701);
    sl::ViewportHandle viewport(77u), otherViewport(78u);
    const sl::BaseStructure* inputs[]={&viewport};
    auto* command=reinterpret_cast<sl::CommandBuffer*>(uintptr_t(0xabcdef));
    assert(!P::Current().observed && P::Current().depth==0);
    {
        P::Scope outer(true,sl::kFeatureDLSS_RR,outerToken,inputs,1,command);
        const auto value=P::Current();
        assert(value.observed && value.frameIndex==700 && value.feature==sl::kFeatureDLSS_RR);
        assert(value.commandBuffer==0xabcdef && value.viewport==77 && value.depth==1);
        assert(value.viewportStatus==P::ViewportStatus::Observed && outerToken.reads==1);
        {
            const sl::BaseStructure* nested[]={&otherViewport};
            P::Scope inner(true,sl::kFeatureDLSS,innerToken,nested,1,nullptr);
            assert(P::Current().frameIndex==701 && P::Current().viewport==78 && P::Current().depth==2);
        }
        assert(P::Current().frameIndex==700 && P::Current().viewport==77);
        {
            const auto reads=innerToken.reads;
            P::Scope inactive(false,sl::kFeatureDLSS,innerToken,
                reinterpret_cast<const sl::BaseStructure* const*>(uintptr_t(1)),999,nullptr);
            assert(!P::Current().observed && innerToken.reads==reads); // Mask outer, do not read inputs/token.
        }
        assert(P::Current().frameIndex==700);
        std::thread worker([&] {
            assert(!P::Current().observed);
            P::Scope local(true,sl::kFeatureDLSS,innerToken,nullptr,0,nullptr);
            assert(P::Current().frameIndex==701);
        });
        worker.join();
        assert(P::Current().frameIndex==700);
        try
        {
            P::Scope failing(true,sl::kFeatureDLSS,innerToken,nullptr,0,nullptr);
            throw std::runtime_error("original API callback exception");
        }
        catch(const std::runtime_error&) {}
        assert(P::Current().frameIndex==700); // Stack unwind restores outer scope.
        innerToken.fail=true;
        unsigned originalCalls=0;
        {
            P::Scope unavailable(true,sl::kFeatureDLSS,innerToken,nullptr,0,nullptr);
            assert(!P::Current().observed);
            ++originalCalls; // Metadata's failing virtual read did not prevent original callback.
        }
        innerToken.fail=false;
        assert(originalCalls==1 && P::Current().frameIndex==700);
    }
    assert(!P::Current().observed && P::Current().depth==0);
    Depth(1,outerToken);
    assert(!P::Current().observed && P::Current().depth==0);

    auto viewportStatus=[&](const sl::BaseStructure* const* array,uint32_t count) {
        P::Scope scope(true,sl::kFeatureDLSS_RR,outerToken,array,count,command);
        assert(P::Current().observed); // Real token and unavailable viewport are distinct facts.
        return P::Current().viewportStatus;
    };
    assert(viewportStatus(nullptr,0)==P::ViewportStatus::Missing);
    assert(viewportStatus(nullptr,1)==P::ViewportStatus::InvalidInput);
    const sl::BaseStructure* nullInput[]={nullptr};
    assert(viewportStatus(nullInput,1)==P::ViewportStatus::InvalidInput);
    assert(viewportStatus(reinterpret_cast<const sl::BaseStructure* const*>(uintptr_t(1)),
                          P::MaxInputs+1)==P::ViewportStatus::InputLimit);
    const sl::BaseStructure* duplicates[]={&viewport,&otherViewport};
    assert(viewportStatus(duplicates,2)==P::ViewportStatus::Ambiguous);
    sl::BaseStructure unknown(sl::StructType{},sl::kStructVersion1);
    unknown.next=&viewport;
    const sl::BaseStructure* chained[]={&unknown};
    assert(viewportStatus(chained,1)==P::ViewportStatus::Observed);
    viewport.structVersion=2;
    assert(viewportStatus(chained,1)==P::ViewportStatus::UnsupportedVersion);
    viewport.structVersion=1;
    viewport.next=&unknown;
    assert(viewportStatus(chained,1)==P::ViewportStatus::ChainLimit);
    viewport.next=nullptr;
    sl::ViewportHandle chain[P::MaxInputNodes+1];
    for(unsigned i=0;i<P::MaxInputNodes;++i) chain[i].next=&chain[i+1];
    // Use unknown headers so multiple known viewports do not terminate before the hard node limit.
    for(auto& node:chain) node.structType={};
    const sl::BaseStructure* tooLong[]={chain};
    assert(viewportStatus(tooLong,1)==P::ViewportStatus::ChainLimit);
    assert(!P::Current().observed);
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-sl-scope-") as name:
            directory = Path(name)
            source = directory / "scope_test.cpp"
            source.write_text(harness)
            executable = directory / "scope_test"
            compiled = subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-pthread", str(source),
                                       "-I", str(HELPER.parent), "-I", str(ROOT / "external/streamline"),
                                       "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
