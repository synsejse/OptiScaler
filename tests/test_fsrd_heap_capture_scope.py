"""Exercise the actual scoped suppression class, including overlapping threads."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STATE = (ROOT / "OptiScaler/State.h").read_text()
TRACKER = (ROOT / "OptiScaler/resource_tracking/ResTrack_dx12.cpp").read_text()


class HeapCaptureScope(unittest.TestCase):
    def test_only_scope_policy_changes_at_descriptor_hook(self):
        self.assertIn("std::atomic<bool> skipHeapCapture { false };", STATE)
        self.assertIn("#include <atomic>", STATE)
        start = TRACKER.index("HRESULT ResTrack_Dx12::hkCreateDescriptorHeap(")
        body = TRACKER[start:TRACKER.index("// try to calculate handle ranges", start)]
        self.assertEqual(body.count("o_CreateDescriptorHeap("), 1)
        self.assertIn("if (ScopedSkipHeapCapture::ShouldSkip())", body)
        self.assertLess(body.index("o_CreateDescriptorHeap("), body.index("ShouldSkip()"))
        self.assertNotIn("State::Instance().skipHeapCapture", TRACKER)

    def test_actual_scope_nested_exception_and_cross_thread(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX to compile heap suppression tests")
        actual = re.search(r"class ScopedSkipHeapCapture\n\{.*?\n\};", STATE, re.S).group()
        declaration = re.search(r"std::atomic<bool> skipHeapCapture \{ false \};", STATE).group()
        harness = r'''
#include <atomic>
#include <barrier>
#include <cassert>
#include <thread>
#include <type_traits>
#include <vector>
class State {
public:
''' + declaration + r'''
 static State& Instance(){static State instance;return instance;}
};
''' + actual + r'''
static_assert(!std::is_copy_constructible_v<ScopedSkipHeapCapture>);
static_assert(!std::is_move_constructible_v<ScopedSkipHeapCapture>);
static_assert(std::is_nothrow_constructible_v<ScopedSkipHeapCapture>);
static_assert(std::is_nothrow_destructible_v<ScopedSkipHeapCapture>);
int main(){
 auto& global=State::Instance().skipHeapCapture;
 assert(!ScopedSkipHeapCapture::ShouldSkip());
 {
  ScopedSkipHeapCapture outer;
  assert(ScopedSkipHeapCapture::ShouldSkip()&&!global.load());
  try {ScopedSkipHeapCapture nested;assert(ScopedSkipHeapCapture::ShouldSkip());throw 1;}
  catch(int){}
  assert(ScopedSkipHeapCapture::ShouldSkip());
 }
 assert(!ScopedSkipHeapCapture::ShouldSkip());
 // A scope must not restore a stale global value over an explicit legacy update.
 {
  ScopedSkipHeapCapture scope;global=true;
 }
 assert(ScopedSkipHeapCapture::ShouldSkip()&&global.load());
 global=false;
 // Two genuinely overlapping allocator scopes must not suppress an unrelated
 // game thread or overwrite one another's restoration in either unwind order.
 std::barrier stages(3);
 auto allocator=[&](bool leaveFirst){
  assert(!ScopedSkipHeapCapture::ShouldSkip());
  {
   ScopedSkipHeapCapture scope;
   stages.arrive_and_wait();stages.arrive_and_wait();
   assert(ScopedSkipHeapCapture::ShouldSkip());
   if(!leaveFirst){stages.arrive_and_wait();stages.arrive_and_wait();}
  }
  assert(!ScopedSkipHeapCapture::ShouldSkip());
  if(leaveFirst){stages.arrive_and_wait();stages.arrive_and_wait();}
 };
 std::thread first(allocator,true),second(allocator,false);
 stages.arrive_and_wait();assert(!ScopedSkipHeapCapture::ShouldSkip()&&!global.load());
 stages.arrive_and_wait();stages.arrive_and_wait();
 assert(!ScopedSkipHeapCapture::ShouldSkip()&&!global.load());
 stages.arrive_and_wait();first.join();second.join();
 assert(!ScopedSkipHeapCapture::ShouldSkip());
 // Explicit global suppression is still visible cross-thread, and clearing it
 // cannot cancel a worker's independent local scope.
 std::barrier legacy(2);
 std::thread worker([&]{
  legacy.arrive_and_wait();assert(ScopedSkipHeapCapture::ShouldSkip());
  {ScopedSkipHeapCapture scope;legacy.arrive_and_wait();legacy.arrive_and_wait();
   assert(!global.load()&&ScopedSkipHeapCapture::ShouldSkip());}
  assert(!ScopedSkipHeapCapture::ShouldSkip());
 });
 global=true;legacy.arrive_and_wait();legacy.arrive_and_wait();
 global=false;legacy.arrive_and_wait();worker.join();
 // Stress temporary scopes while the intentional atomic global switch changes.
 std::vector<std::thread> threads;
 for(unsigned n=0;n<4;++n)threads.emplace_back([&]{
  for(unsigned i=0;i<10000;++i){ScopedSkipHeapCapture a;ScopedSkipHeapCapture b;
   assert(ScopedSkipHeapCapture::ShouldSkip());}
 });
 for(unsigned i=0;i<10000;++i)global=(i&1)!=0;
 for(auto& thread:threads)thread.join();
 global=false;
 assert(!ScopedSkipHeapCapture::ShouldSkip());
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-heap-scope-") as directory:
            path = Path(directory)
            source, executable = path / "scope.cpp", path / "scope"
            source.write_text(harness)
            for flags in (("-O0",), ("-O3", "-ffast-math")):
                result = subprocess.run([compiler, "-std=c++20", "-pthread", "-Wall", "-Wextra",
                                         "-Werror", *flags, str(source), "-o", str(executable)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
