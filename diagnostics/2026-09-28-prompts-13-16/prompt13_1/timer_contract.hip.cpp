#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hip/hip_runtime.h>
#include <hip/hip_version.h>
#include "src/core/json.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

using gufo::json::Value;
constexpr unsigned kThreads=4096, kIterations=65536;
__host__ __device__ unsigned Calculate(unsigned i) {
  unsigned x=i+1;
  for(unsigned j=0;j<kIterations;++j) { x=x*1664525u+1013904223u; x^=x>>13; }
  return x;
}
__global__ void Work(unsigned* out) {
  unsigned i=blockIdx.x*blockDim.x+threadIdx.x;
  if(i<kThreads) out[i]=Calculate(i);
}
double Qpc() {
  LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);
  return 1000.0*static_cast<double>(t.QuadPart)/static_cast<double>(f.QuadPart);
}
Value Api(const char* name,hipError_t status) {
  Value a=Value::object();a["api"]=name;a["code"]=static_cast<int>(status);
  a["name"]=hipGetErrorName(status);a["description"]=hipGetErrorString(status);return a;
}
void Log(Value& a,const char* name,hipError_t status) { a.push_back(Api(name,status)); }
struct Pair { hipEvent_t start{},stop{};bool consumed=true; };
bool Create(Pair& p,unsigned flags,Value& a) {
  auto x=hipEventCreateWithFlags(&p.start,flags);Log(a,"hipEventCreateWithFlags(start)",x);
  auto y=hipEventCreateWithFlags(&p.stop,flags);Log(a,"hipEventCreateWithFlags(stop)",y);
  return x==hipSuccess && y==hipSuccess;
}
void Destroy(Pair& p,Value& a) {
  if(p.start)Log(a,"hipEventDestroy(start)",hipEventDestroy(p.start));
  if(p.stop)Log(a,"hipEventDestroy(stop)",hipEventDestroy(p.stop));
  p={};
}
int main(int argc,char** argv) {
  if(argc!=2)return 2;
  Value root=Value::object(),setup=Value::array(),trials=Value::array();
  int runtime=0,driver=0;hipDeviceProp_t props{};
  Log(setup,"hipRuntimeGetVersion",hipRuntimeGetVersion(&runtime));
  Log(setup,"hipDriverGetVersion",hipDriverGetVersion(&driver));
  Log(setup,"hipGetDeviceProperties",hipGetDeviceProperties(&props,0));
  root["runtime_version"]=runtime;root["driver_version"]=driver;
  root["header_version"]=HIP_VERSION;root["header_githash"]=HIP_VERSION_GITHASH;
  root["arch"]=props.gcnArchName;root["device"]=props.name;
  char module[MAX_PATH]{};GetModuleFileNameA(GetModuleHandleA("amdhip64_7.dll"),module,MAX_PATH);
  root["loaded_hip_module"]=module;
  Value exports=Value::object();
  for(auto name:{"hipEventCreate","hipEventCreateWithFlags","hipEventRecord","hipEventRecordWithFlags","hipEventQuery","hipEventSynchronize","hipEventElapsedTime"})
    exports[name]=GetProcAddress(GetModuleHandleA("amdhip64_7.dll"),name)!=nullptr;
  root["runtime_exports"]=exports;
  hipStream_t stream{};unsigned* device{};
  Log(setup,"hipStreamCreateWithFlags",hipStreamCreateWithFlags(&stream,hipStreamNonBlocking));
  Log(setup,"hipMalloc",hipMalloc(&device,kThreads*sizeof(unsigned)));
  std::vector<unsigned> expected(kThreads),actual(kThreads);
  for(unsigned i=0;i<kThreads;++i)expected[i]=Calculate(i);
  Work<<<16,256,0,stream>>>(device);Log(setup,"warmup kernel launch",hipGetLastError());
  Log(setup,"warmup stream sync",hipStreamSynchronize(stream));
  hipGraph_t graph{};hipGraphExec_t exec{};
  Log(setup,"hipStreamBeginCapture",hipStreamBeginCapture(stream,hipStreamCaptureModeThreadLocal));
  Work<<<16,256,0,stream>>>(device);Log(setup,"captured kernel launch",hipGetLastError());
  Log(setup,"hipStreamEndCapture",hipStreamEndCapture(stream,&graph));
  Log(setup,"hipGraphInstantiate",hipGraphInstantiate(&exec,graph,nullptr,nullptr,0));
  std::vector<std::pair<const char*,unsigned>> configurations={{"standard",hipEventDefault}};
#if defined(hipEventDisableSystemFence) && defined(hipEventReleaseToDevice)
  configurations.push_back({"precision",hipEventDisableSystemFence|hipEventReleaseToDevice});
  root["precision_flags_available"]=true;
#else
  root["precision_flags_available"]=false;
#endif
#ifdef hipEventRecordExternal
  root["record_external_defined"]=true;
#else
  root["record_external_defined"]=false;
#endif
  root["kernel_threads"]=static_cast<int>(kThreads);root["kernel_iterations"]=static_cast<int>(kIterations);
  for(auto [name,flags]:configurations) for(bool reuse:{false,true}) {
    std::array<Pair,4> pool{};bool supported=true;
    if(reuse)for(auto& p:pool)if(!Create(p,flags,setup))supported=false;
    unsigned serial=0;
    auto run=[&](const char* kind,unsigned launches,int trial,bool warmup) {
      Value row=Value::object(),calls=Value::array();
      row["flags_name"]=name;row["flags"]=static_cast<unsigned long long>(flags);
      row["lifecycle"]=reuse ? "pool4" : "unique";row["kind"]=kind;
      row["launches"]=static_cast<int>(launches);row["trial"]=trial;row["warmup"]=warmup;
      Pair unique;auto& pair=reuse?pool[(serial++)%pool.size()]:unique;
      if((!reuse && !Create(pair,flags,calls)) || !supported || !pair.consumed) {
        row["setup_rejected"]=true;Destroy(unique,calls);row["api"]=calls;trials.push_back(row);return;
      }
      pair.consumed=false;
      const double t0=Qpc();
      auto start=hipEventRecord(pair.start,stream);Log(calls,"hipEventRecord(start)",start);
      for(unsigned i=0;i<launches;++i) {
        if(std::string_view(kind)=="eager") {
          Work<<<16,256,0,stream>>>(device);Log(calls,"kernel launch",hipGetLastError());
        } else Log(calls,"hipGraphLaunch",hipGraphLaunch(exec,stream));
      }
      auto stop=hipEventRecord(pair.stop,stream);Log(calls,"hipEventRecord(stop)",stop);
      Log(calls,"hipEventQuery(stop,before completion)",hipEventQuery(pair.stop));
      auto sync=hipEventSynchronize(pair.stop);Log(calls,"hipEventSynchronize(stop)",sync);
      const double wall=Qpc()-t0;
      auto qs=hipEventQuery(pair.start),qe=hipEventQuery(pair.stop);
      Log(calls,"hipEventQuery(start,after completion)",qs);Log(calls,"hipEventQuery(stop,after completion)",qe);
      float elapsed=std::numeric_limits<float>::quiet_NaN();
      auto status=hipEventElapsedTime(&elapsed,pair.start,pair.stop);Log(calls,"hipEventElapsedTime",status);
      row["raw_elapsed_ms"]=std::isfinite(elapsed)?Value(elapsed):Value();
      row["elapsed_finite"]=std::isfinite(elapsed);row["elapsed_positive"]=elapsed>0;
      row["qpc_wall_ms"]=wall;row["elapsed_status"]=static_cast<int>(status);
      row["valid_api_sample"]=start==hipSuccess && stop==hipSuccess && sync==hipSuccess && qs==hipSuccess && qe==hipSuccess && status==hipSuccess && std::isfinite(elapsed) && elapsed>0;
      // Consume before reuse. Precision events do not promise system visibility;
      // the explicit copy below is the correctness dependency, outside timing.
      pair.consumed=sync==hipSuccess && qe==hipSuccess && status!=hipErrorNotReady;
      Log(calls,"correctness hipMemcpy",hipMemcpy(actual.data(),device,kThreads*sizeof(unsigned),hipMemcpyDeviceToHost));
      row["correct"]=actual==expected;
      if(!reuse)Destroy(unique,calls);
      row["api"]=calls;trials.push_back(row);
    };
    for(auto kind:{"eager","graph"}) {
      run(kind,1,-1,true);for(int i=0;i<5;++i)run(kind,1,i,false);
    }
    for(unsigned n:{1u,8u,32u,128u}) {
      run("batch",n,-1,true);for(int i=0;i<3;++i)run("batch",n,i,false);
    }
    if(reuse)for(auto& p:pool)Destroy(p,setup);
  }
  Log(setup,"hipGraphExecDestroy",hipGraphExecDestroy(exec));Log(setup,"hipGraphDestroy",hipGraphDestroy(graph));
  Log(setup,"hipFree",hipFree(device));Log(setup,"hipStreamDestroy",hipStreamDestroy(stream));
  root["setup_cleanup_api"]=setup;root["trials"]=trials;
  std::ofstream(argv[1])<<root.dump()<<'\n';
  std::cout<<"Saved "<<trials.size()<<" trials to "<<argv[1]<<'\n';
}

