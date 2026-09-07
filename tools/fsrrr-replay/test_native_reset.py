"""Actual RESET option/constants builder; GPU conversion is Windows CI/runtime."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


class NativeReset(unittest.TestCase):
    def test_actual_camera_and_wire_constants(self):
        compiler = os.environ.get('CXX') or shutil.which('c++')
        if not compiler:
            self.skipTest('Set CXX for actual C++ option tests')
        with tempfile.TemporaryDirectory(prefix='fsrd-native-replay-') as temporary:
            executable = Path(temporary) / 'test'
            outputs = []
            for flags in (['-O0'], ['-O3']):
                result = subprocess.run([compiler, '-std=c++20', *flags, '-I', str(ROOT / 'external/nlohmann'),
                                         str(HERE / 'native_reset_tests.cpp'), '-o', str(executable)],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                outputs.append(result.stdout)
            self.assertEqual(outputs[0], outputs[1])

    def test_same_production_shader_binding_and_own_fence(self):
        source = (HERE / 'replay.cpp').read_text()
        build = (HERE / 'build.cmd').read_text()
        project = (ROOT / 'OptiScaler/OptiScaler.vcxproj').read_text()
        flags = '-T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect'
        self.assertIn(flags, build)
        self.assertIn(flags, project)
        self.assertIn('FSRDInputConv.hlsl', build)
        self.assertIn('static_assert(sizeof(raw.constants) == 240)', source)
        self.assertIn('nullptr, "raw_hit", "raw_diffuse_albedo", "raw_specular_albedo"', source)
        self.assertIn('list->SetComputeRootConstantBufferView(0,', source)
        self.assertIn('list->SetComputeRootDescriptorTable(1,', source)
        self.assertIn('list->SetComputeRootDescriptorTable(2,', source)
        self.assertLess(source.index('rawInputs = inputs'), source.index('conversion = convertNative('))
        self.assertLess(source.index('conversion = convertNative('), source.index('"Dispatch denoiser"'))
        self.assertLess(source.index('"Dispatch denoiser"'), source.index('convertedReadbacks[name]'))
        self.assertLess(source.index('WaitForSingleObject(event, 45000)'), source.index('writeReadback(convertedReadbacks'))
        self.assertIn('native->providerId != id', source)
        self.assertIn('entry.at("sha256") != sha256', source)
        self.assertIn('ExitProcess(2)', source)


if __name__ == '__main__':
    unittest.main()
