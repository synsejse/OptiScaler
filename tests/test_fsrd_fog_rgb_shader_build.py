"""Fail-closed FXC/native-reflection build gate; no Linux reflection proof.

The actual validator is compiled against controlled reflection fixtures here.
The mandatory MSBuild target runs the same validator on real generated DXBC
using native Windows D3DReflect before compiling OptiScaler.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/fsrrr-rgb-shaders'
NS = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}


class FogRgbShaderBuild(unittest.TestCase):
    def test_native_validation_is_mandatory_before_consumer_compile(self):
        project = ET.parse(ROOT / 'OptiScaler/OptiScaler.vcxproj').getroot()
        target = project.find("m:Target[@Name='CompileFSRDFogRgbShaders']", NS)
        self.assertIsNotNone(target)
        self.assertEqual(target.attrib['BeforeTargets'], 'ClCompile')
        self.assertNotIn('Condition', target.attrib)
        for dependency in ('FSRDFogRgbWrite.hlsl', 'fxc.exe', 'validate.cmd', 'validate.cpp',
                           '$(MSBuildProjectFullPath)'):
            self.assertIn(dependency, target.attrib['Inputs'])
        for output in ('FSRDFogRgbWrite_VS.h', 'FSRDFogRgbWrite_PS.h', 'FSRDFogRgbWrite.validated'):
            self.assertIn(output, target.attrib['Outputs'])
        commands = target.findall('m:Exec', NS)
        self.assertEqual(len(commands), 3)
        for stage, command in zip(('VS', 'PS'), commands[:2]):
            text = command.attrib['Command']
            for flag in ('/Ges', '/O3', '/WX', f'/T {stage.lower()}_5_0',
                         f'/E {stage}Main', '/Fh', f'/Vn FSRDFogRgbWrite_{stage}_cso'):
                self.assertIn(flag, text)
            self.assertIn('fxc.exe', text)
            self.assertIn(f'FSRDFogRgbWrite_{stage}.h', text)
            for unsafe in ('Qstrip', '/Vd', 'IgnoreExitCode', 'ContinueOnError'):
                self.assertNotIn(unsafe, ET.tostring(command, encoding='unicode'))
        self.assertIn('validate.cmd', commands[2].attrib['Command'])
        self.assertNotIn('IgnoreExitCode', commands[2].attrib)
        self.assertNotIn('ContinueOnError', commands[2].attrib)
        actions = [child.tag.rsplit('}', 1)[-1] for child in target]
        self.assertEqual(actions, ['MakeDir', 'Delete', 'Exec', 'Exec', 'Exec', 'Touch'])
        self.assertEqual(target.find('m:Delete', NS).attrib['Files'],
                         target.find('m:Touch', NS).attrib['Files'])
        # Never batch this graphics source through the DXC/SM6 compute target.
        self.assertFalse(any('FSRDFogRgbWrite' in item.attrib.get('Include', '')
                             for item in project.findall('.//m:FSRDShader', NS)))

    def test_native_validator_tool_uses_same_immutable_headers(self):
        source = (TOOLS / 'validate.cpp').read_text()
        command = (TOOLS / 'validate.cmd').read_text()
        for stage in ('VS', 'PS'):
            self.assertIn(f'#include "FSRDFogRgbWrite_{stage}.h"', source)
            self.assertIn(f'Validate(FSRDFogRgbWrite_{stage}_cso, sizeof(FSRDFogRgbWrite_{stage}_cso)', source)
        for check in ('D3DReflect(', 'IsSampleFrequencyShader()', 'D3D11_SHVER_GET_TYPE',
                      'D3D11_SHVER_GET_MAJOR', 'D3D11_SHVER_GET_MINOR'):
            self.assertIn(check, source)
        self.assertNotIn('D3DCompile(', source)
        self.assertIn('vcvars64.bat', command)
        self.assertIn('cl /nologo', command)
        self.assertIn('/WX', command)
        self.assertIn('/link d3dcompiler.lib', command)
        self.assertIn('fsrrr-rgb-shaders-validate.exe\nif errorlevel 1 exit /b 1', command)

    def test_actual_validator_rejects_stub_bad_profile_and_signature(self):
        compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('Set CXX to compile the actual build validator')
        mock = r'''
#pragma once
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
using BYTE=unsigned char;using UINT=unsigned;using HRESULT=int;using BOOL=int;
#define FAILED(x) ((x)<0)
#define IID_PPV_ARGS(x) (x)
#define D3D11_SHVER_GET_TYPE(x) (((x)>>16)&0xffff)
#define D3D11_SHVER_GET_MAJOR(x) (((x)>>4)&0xf)
#define D3D11_SHVER_GET_MINOR(x) ((x)&0xf)
enum {D3D11_SHVER_PIXEL_SHADER=0,D3D11_SHVER_VERTEX_SHADER=1,D3D_NAME_UNDEFINED=0,
 D3D_NAME_VERTEX_ID=6,D3D_NAME_TARGET=64,D3D_REGISTER_COMPONENT_FLOAT32=3,
 D3D_REGISTER_COMPONENT_UINT32=1,D3D_SIT_TEXTURE=2,D3D_SRV_DIMENSION_TEXTURE2D=4,
 D3D_RETURN_TYPE_FLOAT=5};
inline int _stricmp(const char* a,const char* b){while(*a&&*b){int x=std::tolower((unsigned char)*a++);
 int y=std::tolower((unsigned char)*b++);if(x!=y)return x-y;}return *a-*b;}
struct D3D11_SHADER_DESC{UINT Version=0,InputParameters=1,OutputParameters=1,BoundResources=1;};
struct D3D11_SIGNATURE_PARAMETER_DESC{const char* SemanticName=nullptr;UINT SemanticIndex=0;
 UINT SystemValueType=0,ComponentType=3;BYTE Mask=3;UINT Stream=0;};
struct D3D11_SHADER_INPUT_BIND_DESC{UINT Type=2,BindPoint=0,BindCount=1,Dimension=4,ReturnType=5;};
namespace Fake{
struct Shader{D3D11_SHADER_DESC desc;D3D11_SIGNATURE_PARAMETER_DESC input,output;
 D3D11_SHADER_INPUT_BIND_DESC texture;bool frequency=false,reflectionFail=false,reflectionNull=false;
 bool descFail=false,inputFail=false,outputFail=false,textureFail=false;};
inline Shader vs,ps;inline unsigned alive=0;
inline void Reset(){vs={};ps={};vs.desc={0x10050,1,2,0};ps.desc={0x50,1,1,1};
 vs.input={"SV_VertexID",0,6,1,1,0};ps.input={"TEXCOORD",0,0,3,3,0};
 ps.output={"SV_Target",0,64,3,15,0};ps.frequency=true;}
}
struct ID3D11ShaderReflection{Fake::Shader* s;explicit ID3D11ShaderReflection(Fake::Shader* p):s(p){++Fake::alive;}
 ~ID3D11ShaderReflection(){--Fake::alive;}
 HRESULT GetDesc(D3D11_SHADER_DESC* p){*p=s->desc;return s->descFail?-1:0;}
 BOOL IsSampleFrequencyShader(){return s->frequency;}
 HRESULT GetInputParameterDesc(UINT i,D3D11_SIGNATURE_PARAMETER_DESC* p){assert(i==0);*p=s->input;return s->inputFail?-1:0;}
 HRESULT GetOutputParameterDesc(UINT i,D3D11_SIGNATURE_PARAMETER_DESC* p){assert(i==0);*p=s->output;return s->outputFail?-1:0;}
 HRESULT GetResourceBindingDesc(UINT i,D3D11_SHADER_INPUT_BIND_DESC* p){assert(i==0);*p=s->texture;return s->textureFail?-1:0;}};
inline HRESULT D3DReflect(const void* bytes,size_t size,ID3D11ShaderReflection** out){
 assert(bytes);bool pixel=static_cast<const BYTE*>(bytes)[0]==0x50;assert(size==(pixel?44u:40u));
 auto& s=pixel?Fake::ps:Fake::vs;if(s.reflectionFail)return -1;if(s.reflectionNull)return 0;
 *out=new ID3D11ShaderReflection(&s);return 0;}
namespace Microsoft::WRL{template<class T>class ComPtr{T* p=nullptr;public:
 ~ComPtr(){delete p;}T* operator->()const{return p;}explicit operator bool()const{return p!=nullptr;}
 T** operator&(){assert(!p);return &p;}};}
'''
        harness = r'''
#define main ValidateGeneratedMain
#include "validate.cpp"
#undef main
int main(){
 Fake::Reset();assert(ValidateGeneratedMain()==0&&!Fake::alive);
 // The installed Wine stub returns false, including for the genuine PS.
 Fake::Reset();Fake::ps.frequency=false;assert(ValidateGeneratedMain()!=0&&!Fake::alive);
 for(bool pixel:{false,true})for(int bad=0;bad<10;++bad){Fake::Reset();auto& s=pixel?Fake::ps:Fake::vs;
  switch(bad){case 0:s.reflectionFail=true;break;case 1:s.reflectionNull=true;break;
   case 2:s.descFail=true;break;case 3:s.desc.Version^=0x10000;break;case 4:s.desc.Version+=0x10;break;
   case 5:s.desc.Version+=1;break;case 6:s.frequency=!pixel;break;case 7:s.desc.InputParameters=2;break;
   case 8:s.desc.OutputParameters=3;break;case 9:s.desc.BoundResources=2;break;}
  assert(ValidateGeneratedMain()!=0&&!Fake::alive);}
 for(bool pixel:{false,true})for(int bad=0;bad<7;++bad){Fake::Reset();auto& s=pixel?Fake::ps:Fake::vs;
  switch(bad){case 0:s.inputFail=true;break;case 1:s.input.SemanticIndex=1;break;
   case 2:s.input.SystemValueType=99;break;case 3:s.input.ComponentType=99;break;
   case 4:s.input.Mask=15;break;case 5:s.input.Stream=1;break;
   case 6:if(pixel)s.input.SemanticName=nullptr;else s.input.Mask=0;break;}
  assert(ValidateGeneratedMain()!=0&&!Fake::alive);}
 for(int bad=0;bad<13;++bad){Fake::Reset();auto& s=Fake::ps;
  switch(bad){case 0:s.input.SemanticName="SV_Position";break;case 1:s.textureFail=true;break;
   case 2:s.texture.Type=99;break;case 3:s.texture.BindPoint=1;break;case 4:s.texture.BindCount=2;break;
   case 5:s.texture.Dimension=99;break;case 6:s.texture.ReturnType=99;break;case 7:s.outputFail=true;break;
   case 8:s.output.SystemValueType=99;break;case 9:s.output.SemanticIndex=1;break;
   case 10:s.output.ComponentType=99;break;case 11:s.output.Mask=7;break;case 12:s.output.Stream=1;break;}
  assert(ValidateGeneratedMain()!=0&&!Fake::alive);}
 Fake::Reset();assert(ValidateGeneratedMain()==0&&!Fake::alive);
}
'''
        with tempfile.TemporaryDirectory(prefix='fsrd-rgb-shader-build-') as temporary:
            temp = Path(temporary)
            files = {'windows.h': mock, 'd3dcompiler.h': '#include "windows.h"\n',
                     'd3d11shader.h': '#include "windows.h"\n',
                     'wrl/client.h': '#include "windows.h"\n',
                     'FSRDFogRgbWrite_VS.h': 'const BYTE FSRDFogRgbWrite_VS_cso[40]={0x56};\n',
                     'FSRDFogRgbWrite_PS.h': 'const BYTE FSRDFogRgbWrite_PS_cso[44]={0x50};\n',
                     'test.cpp': '#include <initializer_list>\n' + harness}
            for name, content in files.items():
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            for flags in (['-O0'], ['-O3', '-ffast-math']):
                result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', *flags,
                                         '-I', str(temp), '-I', str(TOOLS), str(temp / 'test.cpp'),
                                         '-o', str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(temp / 'test')], text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
