"""A lighting metadata candidate must never delay the independent Fog capture."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'OptiScaler/upscalers/ffx/FSRDCyberpunkFogProbe.cpp').read_text()
PREPARE = SOURCE.split('std::shared_ptr<CapturePlan> PrepareCapture(', 1)[1].split('\nvoid ', 1)[0]
CANDIDATE = PREPARE[PREPARE.index('    if (lightingRequested.load())'):
                    PREPARE.index('    if (earlyRequested.load())')]


class FogCapturePairing(unittest.TestCase):
    def test_actual_candidate_block_never_skips_current_fog_draw(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX to compile actual Fog candidate block')
        harness = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include "json.hpp"
using Json = nlohmann::json;
struct CapturePlan { Json provenance = Json::object(); };
struct Registry { std::mutex mutex; Json lightingCaptureSource; };
Registry registry;
Registry& Data() { return registry; }
std::atomic<bool> lightingRequested{false};
// Keep the previous timer ABI so the actual old early-return fails this test
// at runtime, rather than merely failing to compile.
std::atomic<uint64_t> lightingRequestedAt{1000};
uint64_t GetTickCount64() { return 1001; }
std::shared_ptr<CapturePlan> CurrentFogCapture() {
    auto plan = std::make_shared<CapturePlan>();
'''
        harness += CANDIDATE
        harness += r'''
    return plan;
}
int main() {
    // No paired request: independent Fog capture remains admissible.
    auto p = CurrentFogCapture();
    assert(p && p->provenance.empty());
    // Same-frame Fog records first; lighting is pending, not yet published.
    lightingRequested = true;
    p = CurrentFogCapture();
    assert(p && p->provenance.empty());
    // Publication later cannot retroactively authenticate or mutate that Fog.
    registry.lightingCaptureSource = {{"scope", 17}, {"metadata", {{"frame", 99}}}};
    const auto published = registry.lightingCaptureSource;
    assert(p->provenance.empty());
    auto later = CurrentFogCapture();
    assert(later && later->provenance["lighting_recording_candidate"]["scope"] == 17);
    assert(later->provenance["lighting_recording_candidate"]["pairing_authority"] ==
           "candidate metadata only; current frame/camera match not asserted");
    assert(registry.lightingCaptureSource == published);
    // The source is optional even when its request failed, including long ago.
    registry.lightingCaptureSource = nullptr;
    lightingRequestedAt = 0;
    assert(CurrentFogCapture());
    lightingRequested = false;
    registry.lightingCaptureSource = published;
    p = CurrentFogCapture();
    assert(p && p->provenance.empty());
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-fog-candidate-') as tmp:
            source, binary = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            source.write_text(harness)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', *flags,
                                '-I', str(ROOT / 'external/nlohmann'), str(source), '-o', str(binary)],
                               check=True)
                subprocess.run([str(binary)], check=True)

    def test_candidate_cannot_supply_gpu_inputs_or_frame_authority(self):
        for forbidden in ('return', 'GetTickCount64', 'lightingRequestedAt', 'WantsEarlyGuideCapture',
                          'GetEarlyGuideStatus', 'CopyTexture', 'Dispatch', 'Wait', 'current_inputs',
                          'depthMetadata', 'FSRDSubmission'):
            self.assertNotIn(forbidden, CANDIDATE)
        self.assertIn('source = data.lightingCaptureSource;', CANDIDATE)
        self.assertIn('candidate metadata only; current frame/camera match not asserted', CANDIDATE)
        self.assertIn('PrepareFogDepth(*plan, nativeCaller)', PREPARE)
        depth = SOURCE.split('void PrepareFogDepth(', 1)[1].split('\nvoid ', 1)[0]
        self.assertIn('FSRDCyberpunkEarlyGuides::Describe(', depth)
        self.assertIn('plan.provenance["current_inputs"] = plan.depthMetadata;', depth)
        self.assertNotIn('lightingCaptureSource', depth)


if __name__ == '__main__':
    unittest.main()
