"""Source-policy checks for the opt-in, Windows-only authored-guide recorder.

These checks do not execute D3D12 or claim runtime shader/root compatibility;
the exact constant-packing C++ harness is in test_fsrd_guide_constants.py.
"""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "OptiScaler/upscalers/ffx"
SOURCE = (BASE / "FSRDCyberpunkGuidePass.cpp").read_text()
HEADER = (BASE / "FSRDCyberpunkGuidePass.h").read_text()


class PrivateGuidePass(unittest.TestCase):
    def test_exact_runtime_shader_is_owned_and_authenticated_before_pipeline(self):
        self.assertIn('ShaderBytes = 5824', SOURCE)
        self.assertEqual(re.findall(r'"([a-f0-9]{64})"', SOURCE), [
            "a4bcbce1667fb6f3e130a18482e6088e2835d0028582a52e5a4668f7fae1b7ee"])
        self.assertIn('std::memcmp(bytes.data(), "DXBC", 4)', SOURCE)
        self.assertIn('declared != bytes.size()', SOURCE)
        self.assertIn('BCRYPT_SHA256_ALGORITHM', SOURCE)
        own = SOURCE.index('const std::vector<std::byte> ownedShader')
        authenticate = SOURCE.index('Require(Authenticate(ownedShader)')
        pipeline = SOURCE.index('CreateComputePipelineState')
        self.assertLess(own, authenticate)
        self.assertLess(authenticate, pipeline)
        for forbidden in ('ifstream', 'fopen(', 'CreateFile', 'GetModuleHandle(nullptr)',
                          'RequestState', 'ReadProcessMemory', 'Config::', 'State::'):
            self.assertNotIn(forbidden, SOURCE)

    def test_active_optional_inputs_and_malformed_constants_are_refused(self):
        prepare = SOURCE[SOURCE.index('std::shared_ptr<Work> Prepare'):SOURCE.index('bool Work::Record')]
        self.assertIn('pass[4] == 0 && pass[6] == 0', prepare)
        self.assertIn('PackPass(passSource, checkedPass) && checkedPass == pass', prepare)
        self.assertIn('passSource.noVMode = std::bit_cast<int32_t>(pass[5])', prepare)
        self.assertIn('passSource.extraSpecularScaleBits = pass[7]', prepare)
        self.assertLess(prepare.index('checkedPass == pass'), prepare.index('CreateRootSignature'))
        self.assertIn('SharedAlignedBytes = 1792', SOURCE)
        self.assertIn('D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT', SOURCE)
        self.assertIn('std::memcpy(mapped, shared.data(), sizeof(shared))', SOURCE)

    def test_exact_source_descriptors_are_copied_not_guessed(self):
        self.assertIn('source.resource && source.heap && source.descriptor.ptr', SOURCE)
        self.assertIn('OnDevice(source.resource.Get(), device) && OnDevice(source.heap.Get(), device)', SOURCE)
        self.assertIn('heap.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_NONE', SOURCE)
        self.assertIn('(source.descriptor.ptr - begin) / stride < heap.NumDescriptors', SOURCE)
        self.assertIn('constexpr UINT slots[] = { 0, 1, 2, 4 }', SOURCE)
        self.assertIn('CopyDescriptorsSimple(1, descriptorAt(slots[i]), sources[i].descriptor', SOURCE)
        self.assertIn('b6 transparency and extra-specular flags MUST be', HEADER)
        self.assertIn('ALTERNATE STENCIL SRV', HEADER)

    def test_inactive_srv_types_and_native_output_formats(self):
        self.assertIn('CreateShaderResourceView(nullptr, &nullTexture, descriptorAt(3))', SOURCE)
        self.assertIn('CreateShaderResourceView(nullptr, &nullTexture, descriptorAt(5))', SOURCE)
        self.assertIn('nullExposure.ViewDimension = D3D12_SRV_DIMENSION_BUFFER', SOURCE)
        self.assertIn('nullExposure.Buffer.StructureByteStride = 28', SOURCE)
        self.assertIn('CreateShaderResourceView(nullptr, &nullExposure, descriptorAt(6))', SOURCE)
        self.assertIn('i < 2 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT', SOURCE)
        self.assertIn('allocation.SizeInBytes <= MaxOutputBytes - allocated', SOURCE)
        self.assertIn('MaxOutputBytes = 256ull * 1024 * 1024', SOURCE)

    def test_retention_precedes_every_command_and_has_no_owner_cycle(self):
        record = SOURCE[SOURCE.index('bool Work::Record'):]
        retained = record.index('FSRDSubmission::Retain(')
        commands = list(re.finditer(r'list->(?:Set\w+|Dispatch|ResourceBarrier)\(', record))
        self.assertTrue(commands)
        self.assertTrue(all(retained < command.start() for command in commands))
        self.assertIn('Require(bool(retained)', record)
        self.assertIn('_impl->attempted.exchange(true)', record)
        impl = SOURCE[SOURCE.index('struct Work::Impl'):SOURCE.index('Work::Work')]
        self.assertNotIn('Ticket', impl)
        self.assertIn('std::array<SourceView, 4> sources', impl)
        self.assertIn('std::array<ComPtr<ID3D12Resource>, 3> outputs', impl)

    def test_record_changes_only_private_compute_state_and_private_output_states(self):
        record = SOURCE[SOURCE.index('bool Work::Record'):]
        for forbidden in ('CopyTextureRegion', 'DrawInstanced', 'OMSetRenderTargets',
                          'SetGraphicsRoot', 'RSSet', 'WaitFor', 'ExecuteCommandLists',
                          'Transition.pResource = _impl->sources'):
            self.assertNotIn(forbidden, record)
        self.assertIn('Transition.pResource = _impl->outputs[i].Get()', record)
        self.assertIn('Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS', record)
        self.assertIn('Transition.StateAfter = Readable', record)
        self.assertIn('D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE', SOURCE)
        self.assertIn('predication, active queries, render passes and bundles', HEADER)
        self.assertIn('restore engine roots/heaps/original PSO even when this returns false', HEADER)
        self.assertIn('no partial-coverage dependency on old contents', record)

    def test_windows_project_registers_only_source_not_proprietary_shader_blob(self):
        ns = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
        project = ET.parse(ROOT / 'OptiScaler/OptiScaler.vcxproj')
        includes = {node.attrib['Include'] for node in project.findall('.//m:ClInclude[@Include]', ns)}
        compiles = {node.attrib['Include'] for node in project.findall('.//m:ClCompile[@Include]', ns)}
        self.assertIn(r'upscalers\ffx\FSRDCyberpunkGuidePass.h', includes)
        self.assertIn(r'upscalers\ffx\FSRDCyberpunkGuideConstants.h', includes)
        self.assertIn(r'upscalers\ffx\FSRDCyberpunkGuidePass.cpp', compiles)
        self.assertFalse(any('dlss_convert.dxil' in value for value in includes | compiles))


if __name__ == '__main__':
    unittest.main()
