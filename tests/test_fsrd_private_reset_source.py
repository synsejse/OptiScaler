"""Compile the real CPU-source adapter using synthetic metadata and native camera words.

The camera words already live in test_fsrd_reset_camera.py. Pointer/frame identities
here are synthetic; this test grants no resource lifetime or GPU ordering proof.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "OptiScaler/upscalers/ffx"


class PrivateResetSource(unittest.TestCase):
    def test_source_is_cpu_only_and_compares_exact_authored_evidence(self):
        source = (BASE / "FSRDCyberpunkPrivateResetSource.h").read_text()
        for forbidden in ("ReadProcessMemory", "ID3D12", "NVSDK_", "State::", "Config::", "GetLast"):
            self.assertNotIn(forbidden, source)
        self.assertIn("camera == other.camera", source)
        self.assertIn("CyberpunkResetCamera::Build(", source)
        self.assertIn("explicitResetDelta", source)

    def test_temporal_access_is_additive_and_does_not_invent_history_or_time(self):
        source = (BASE / "FSRDCyberpunkPrivateResetSource.h").read_text()
        self.assertIn('auto& source = result.rawSnapshot', source)
        self.assertIn('TemporalSource ParseTemporal(const Json& metadata, float explicitCurrentDelta)', source)
        self.assertIn('Parse(metadata, explicitCurrentDelta)', source)
        self.assertIn('Parameters\n// still hold the old RESET template', source)
        legacy = source.split('inline Source Parse(', 1)[1].split('struct TemporalSource', 1)[0]
        self.assertNotIn('camera.at("history")', legacy)
        for forbidden in ('MillisecondsNow', 'chrono::', '16.67', 'previous->', 'sessionEpoch'):
            self.assertNotIn(forbidden, source)

    def test_compiled_sources_pairing_and_malformed_fields(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        if not compiler:
            self.skipTest("Set CXX for the real CPU-source adapter test")
        harness = r'''
#include "FSRDCyberpunkPrivateResetSource.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace FSRD::CyberpunkPrivateResetSource;
template<size_t N>
Json matrix(uint32_t offset,const std::array<uint32_t,N>& values,unsigned columns=4){
 Json rows=Json::array();
 for(unsigned r=0;r<N/columns;++r){Json row=Json::array();
  for(unsigned c=0;c<columns;++c)row.push_back(values[r*columns+c]);rows.push_back(row);}
 return {{"status","CPU_value_present"},{"view_offset",offset},
  {"layout","consecutive_float32_rows"},{"repeated_source_words_equal",true},
  {"all_finite",true},{"source_uint32_rows",rows}};
}
Json scalar(uint32_t offset,uint32_t value,bool bits=false){
 Json result={{"status","CPU_value_present"},{"view_offset",offset}};
 result[bits?"source_bits":"source_value"]=value;return result;
}
Json fixture(){
 const std::array<uint32_t,16> v={1052756518,977788298,1064131678,0,
  3211615332,966924020,1052756514,0,898473216,1065353210,3126307840,0,
  3302796298,3262674531,1161996442,1065353216};
 const std::array<uint32_t,16> iv={1052756518,3211615332,898473216,0,
  977788298,966924020,1065353210,0,1064131678,1052756514,3126307840,0,
  3305842410,3308206053,1115773974,1065353216};
 const std::array<uint32_t,16> p={1066856116,0,0,0,0,1074145668,0,0,
  975385395,982935142,1065353226,1065353216,0,0,3164854039,0};
 const std::array<uint32_t,16> pd={1066856116,0,0,0,0,1074145668,0,0,
  975385395,982935142,3047161856,1065353216,0,0,1017370391,0};
 Json camera={{"schema","optiscaler.fsr_rr.early_camera_sources.v2"},
  {"status","current_view_CPU_sources_only"},{"view_pointer_unchanged",true},
  {"frame_id_virtual_route",{{"object_address",0x2000},{"target_rva",0x18ec810},
   {"status","image_local_target_observed"},{"explicit_frame_id_source",{
    {"status","CPU_value_present"},{"getter_rva",0x18ec810},
    {"source_object_offset",0x1b0},{"byte_size",4},{"repeated_source_fields_equal",true},
    {"semantics","CPU_source_for_later_explicit_Streamline_frame_ID"},
    {"source_address",0x21b0},{"source_value",209225}}}}},
  {"matrices",{{"native_view",matrix(0xc0,v)},{"inverse_native_view",matrix(0x180,iv)},
   {"native_projection_jittered",matrix(0x200,p)},
   {"depth_converted_projection_jittered",matrix(0x360,pd)}}},
  {"authored_lens_offset",matrix(0xa0,std::array<uint32_t,2>{0,0},2)},
  {"jitter_x",scalar(0x3e0,1053556736,true)},
  {"jitter_y",scalar(0x3e4,1054048256,true)},
  {"native_jitter_width",scalar(0x3e8,1280)},
  {"native_jitter_height",scalar(0x3ec,720)},
  {"projection_flags",scalar(0x3f4,4)},
  {"motion_scale",{{"status","current_property_CPU_values"},
   {"repeated_source_fields_equal",true},{"defaults_used",false},{"section","DLSS"},
   {"names",Json::array({"MvecScaleX","MvecScaleY"})},
   {"value_rvas",Json::array({0x3464dc0,0x3464e10})},
   {"semantics","native_SL_normalized_motion_multiplier"},
   {"source_bits",Json::array({std::bit_cast<uint32_t>(1.25f),std::bit_cast<uint32_t>(-.5f)})}}},
  {"unconsumed_source_evidence",{{"explicitly_unproven",true}}}};
 return {{"schema","optiscaler.fsr_rr.early_guide_availability.v1"},
  {"status","CPU_metadata_only"},{"view",0x1000},{"view_dimensions",Json::array({1280,720})},
  {"camera_provenance",camera},{"graph_position",12},{"resource_refs",1}};
}
void reject(const Json& value,float dt=17.25f){
 bool failed=false;try{(void)Parse(value,dt);}catch(const std::exception&){failed=true;}
 assert(failed);
}
int main(){
 assert(Address(Json(0x1000))==0x1000 && Address(Json(uint64_t(0x1000)))==0x1000);
 auto good=fixture();auto a=Parse(good,17.25f);
 assert(a.view==0x1000 && a.object==0x2000 && a.frame==209225);
 assert(a.width==1280 && a.height==720 && a.camera==good["camera_provenance"]);
 assert(a.parameters.frameIndex==209225 && a.parameters.deltaMilliseconds==17.25f);
 assert(a.parameters.conversionFlags==69 && a.parameters.dispatchFlags==3);
 assert(a.parameters.motionScale[0]==1.25f && a.parameters.motionScale[1]==-.5f);
 assert((a.motionScale==std::array<float,2>{1.25f,-.5f}));
 assert(a.rawSnapshot.width==1280&&a.rawSnapshot.height==720&&a.rawSnapshot.jitterWidth==1280&&a.rawSnapshot.jitterHeight==720);
 assert((a.rawSnapshot.projectionFlags==4&&a.rawSnapshot.lensOffset==std::array<uint32_t,2>{}));
 assert((a.rawSnapshot.jitterPixels==std::array<uint32_t,2>{1053556736,1054048256}));
 for(const auto& item:{std::pair{"native_view",&a.rawSnapshot.nativeView},
                     std::pair{"inverse_native_view",&a.rawSnapshot.inverseNativeView},
                     std::pair{"native_projection_jittered",&a.rawSnapshot.nativeProjection},
                     std::pair{"depth_converted_projection_jittered",&a.rawSnapshot.depthProjection}})
  for(unsigned i=0;i<16;++i)assert((*item.second)[i]==good["camera_provenance"]["matrices"][item.first]["source_uint32_rows"][i/4][i%4]);
 assert(std::bit_cast<uint32_t>(a.parameters.nearPlane)==1017370378);
 assert(std::bit_cast<uint32_t>(a.parameters.farPlane)==1182995065u);
 assert(a.SameFrame(Parse(good,18.25f))); // Explicit RESET timing is not native frame identity.
 auto b=good;b["graph_position"]=99;b["resource_refs"]=7;
 assert(a.SameFrame(Parse(b,17.25f))); // Different graph/use metadata, same exact authored source.
 b=good;b["view"]=0x3000;assert(!a.SameFrame(Parse(b,17.25f)));
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["object_address"]=0x4000;
 b["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["source_address"]=0x41b0;
 assert(!a.SameFrame(Parse(b,17.25f)));
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["source_value"]=209226;
 assert(!a.SameFrame(Parse(b,17.25f)));
 b=good;b["camera_provenance"]["motion_scale"]["source_bits"][0]=std::bit_cast<uint32_t>(1.f);
 assert(!a.SameFrame(Parse(b,17.25f)));
 b=good;b["camera_provenance"]["unconsumed_source_evidence"]["explicitly_unproven"]=false;
 assert(!a.SameFrame(Parse(b,17.25f))); // No convenient-subset equality.
 auto different=a;different.width++;assert(!a.SameFrame(different));
 different=a;different.height++;assert(!a.SameFrame(different));

 for(const Json& invalid:{Json(-1),Json(0),Json(4096.0),Json(4096.5),Json(true),Json("4096"),Json()}){
  b=good;b["view"]=invalid;reject(b);
  b=good;b["camera_provenance"]["frame_id_virtual_route"]["object_address"]=invalid;reject(b);
 }
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["object_address"]=UINTPTR_MAX-0x100;reject(b);
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["source_address"]=8624.0;reject(b);
 for(const char* key:{"schema","status","view","view_dimensions","camera_provenance"}){
  b=good;b.erase(key);reject(b);
 }
 for(const Json& word:{Json(-1),Json(uint64_t(UINT32_MAX)+1),Json(1280.0),Json(true),Json()}){
  b=good;b["view_dimensions"][0]=word;reject(b);
  b=good;b["camera_provenance"]["matrices"]["native_view"]["source_uint32_rows"][0][0]=word;reject(b);
  b=good;b["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["source_value"]=word;reject(b);
 }
 for(const char* matrixKey:{"native_view","inverse_native_view","native_projection_jittered","depth_converted_projection_jittered"}){
  for(uint32_t nanInf:{0x7f800000u,0xff800000u,0x7fc00000u}){
   b=good;b["camera_provenance"]["matrices"][matrixKey]["source_uint32_rows"][0][0]=nanInf;reject(b);
  }
  b=good;b["camera_provenance"]["matrices"][matrixKey]["source_uint32_rows"].erase(0);reject(b);
  b=good;b["camera_provenance"]["matrices"][matrixKey]["source_uint32_rows"][0].push_back(0);reject(b);
  b=good;b["camera_provenance"]["matrices"][matrixKey]["layout"]="column_major";reject(b);
  b=good;b["camera_provenance"]["matrices"][matrixKey]["view_offset"]=0;reject(b);
  b=good;b["camera_provenance"]["matrices"][matrixKey]["repeated_source_words_equal"]=false;reject(b);
  b=good;b["camera_provenance"]["matrices"][matrixKey]["all_finite"]=false;reject(b);
 }
 b=good;b["camera_provenance"]["view_pointer_unchanged"]=false;reject(b);
 b=good;b["camera_provenance"]["projection_flags"]["source_value"]=256;reject(b);
 b=good;b["camera_provenance"]["native_jitter_width"]["source_value"]=1279;reject(b);
 b=good;b["camera_provenance"]["motion_scale"]["defaults_used"]=true;reject(b);
 b=good;b["camera_provenance"]["motion_scale"]["repeated_source_fields_equal"]=false;reject(b);
 b=good;b["camera_provenance"]["motion_scale"]["source_bits"][0]=0x7f800000u;reject(b);
 b=good;b["camera_provenance"]["motion_scale"]["source_bits"].push_back(0);reject(b);
 b=good;b["camera_provenance"]["motion_scale"]["value_rvas"][0]=0;reject(b);
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["target_rva"]=0;reject(b);
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["getter_rva"]=0;reject(b);
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["repeated_source_fields_equal"]=false;reject(b);
 b=good;b["camera_provenance"]["frame_id_virtual_route"]["explicit_frame_id_source"]["source_address"]=0x21b1;reject(b);
 reject(good,0);reject(good,-1);reject(good,std::bit_cast<float>(0x7f800000u));
 // Old RESET captures have no new history requirement. Temporal access must
 // reject that same missing observation, not turn it into native reset=false.
 const auto temporalReject=[](const Json& input,float delta=17.25f){
  bool failed=false;try{(void)ParseTemporal(input,delta);}catch(const std::exception&){failed=true;}assert(failed);};
 temporalReject(good);
 auto temporal=good;
 temporal["camera_provenance"]["history"]={{"status","CPU_value_present"},{"view_offset",0xef0},
  {"semantics","native_SL_reset_equals_zero"},{"source_byte",1},
  {"producer_reset_candidate",false},{"repeated_source_fields_equal",true}};
 for(unsigned byte:{0u,1u,2u,255u}){
  auto sample=temporal;sample["camera_provenance"]["history"]["source_byte"]=byte;
  sample["camera_provenance"]["history"]["producer_reset_candidate"]=byte==0;
  const auto value=ParseTemporal(sample,18.25f);
  assert(value.nativeResetRequested==(byte==0));assert(value.current.SameFrame(Parse(sample,18.25f)));
  assert(value.current.rawSnapshot.nativeView==a.rawSnapshot.nativeView&&value.current.rawSnapshot.depthProjection==a.rawSnapshot.depthProjection);
  assert(value.current.motionScale==a.motionScale);
  assert(value.current.parameters.conversionFlags==69&&value.current.parameters.dispatchFlags==3);
  assert(value.current.parameters.previousView==value.current.rawSnapshot.nativeView);
  assert(value.current.parameters.deltaMilliseconds==18.25f); // Explicit caller time, not native timing.
 }
 for(const char* key:{"status","view_offset","semantics","source_byte","producer_reset_candidate","repeated_source_fields_equal"}){
  auto sample=temporal;sample["camera_provenance"]["history"].erase(key);temporalReject(sample);
  (void)Parse(sample,17.25f); // Malformed optional history never became a legacy RESET requirement.
 }
 for(const auto& invalid:{Json(-1),Json(256),Json(1.0),Json(true),Json("1"),Json(),Json(uint64_t(UINT32_MAX)+1)}){
  auto sample=temporal;sample["camera_provenance"]["history"]["source_byte"]=invalid;temporalReject(sample);
 }
 for(const auto& invalid:{Json(false),Json(1),Json(1.0),Json("true"),Json()}){
  auto sample=temporal;sample["camera_provenance"]["history"]["repeated_source_fields_equal"]=invalid;temporalReject(sample);
 }
 for(const auto& invalid:{Json(true),Json(0),Json(0.0),Json("false"),Json()}){
  auto sample=temporal;sample["camera_provenance"]["history"]["producer_reset_candidate"]=invalid;temporalReject(sample);
 }
 for(const auto& invalid:{Json(0xef1),Json(3824.0),Json(true),Json()}){
  auto sample=temporal;sample["camera_provenance"]["history"]["view_offset"]=invalid;temporalReject(sample);
 }
 b=temporal;b["camera_provenance"]["history"]["status"]="unavailable";temporalReject(b);
 b=temporal;b["camera_provenance"]["history"]["semantics"]="effective_NGX_reset";temporalReject(b);
 b=temporal;b["camera_provenance"]["history"]["source_byte"]=0;temporalReject(b); // Wrong exact reset expression.
 b=temporal;b["camera_provenance"]["history"]=nullptr;temporalReject(b);
 temporalReject(temporal,0);temporalReject(temporal,-1);temporalReject(temporal,std::bit_cast<float>(0x7fc00001u));
 auto owned=ParseTemporal(temporal,17.25f);const auto snapshot=owned.current.rawSnapshot.nativeView;
 temporal["camera_provenance"]["matrices"]["native_view"]["source_uint32_rows"][0][0]=0;
 assert(owned.current.rawSnapshot.nativeView==snapshot); // Owned raw copy, not a JSON reference.
 std::cout<<"private RESET source tests passed\n";
}
'''
        with tempfile.TemporaryDirectory(prefix="fsrd-private-reset-source-") as directory:
            path = Path(directory)
            (path / "test.cpp").write_text(harness)
            for flags in (("-O0",), ("-O3", "-ffast-math")):
                subprocess.run([compiler, "-std=c++20", *flags, "-I", str(BASE),
                                "-I", str(ROOT / "external/nlohmann"), str(path / "test.cpp"),
                                "-o", str(path / "test")], check=True, capture_output=True, text=True)
                result = subprocess.run([str(path / "test")], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("private RESET source tests passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
