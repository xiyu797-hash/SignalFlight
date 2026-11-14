#include "sf/pipeline.hpp"
#include <complex>
namespace sf {
namespace {
constexpr double PI=3.141592653589793238462643383279502884;

// A reference DFT deliberately retained to check numerical correctness and
// measure the benefit of the optimized path independently of networking.
double reference_centroid(const std::vector<uint8_t>& iq,size_t bins){
 double weighted=0,total=0;
 for(size_t k=0;k<bins;++k){
  double re=0,im=0;
  for(size_t t=0;t<bins;++t){
   const double ar=read_float_le(iq.data()+t*8),ai=read_float_le(iq.data()+t*8+4);
   const double a=-2.0*PI*double(k*t)/double(bins);
   re+=ar*std::cos(a)-ai*std::sin(a);
   im+=ar*std::sin(a)+ai*std::cos(a);
  }
  const double power=re*re+im*im;weighted+=k*power;total+=power;
 }
 return total>0?weighted/(bins*total):0;
}

// 64-point radix-2 FFT, O(N log N) rather than reference O(N^2).
// This is a fixed-size DSP demonstration (not a production FFT library).
double fft64_centroid(const std::vector<uint8_t>& iq){
 using Complex=std::complex<double>;
 std::array<Complex,64> a{};
 for(size_t i=0;i<64;++i)
  a[i]={read_float_le(iq.data()+i*8),read_float_le(iq.data()+i*8+4)};
 // Bit-reversed permutation.
 for(size_t i=1,j=0;i<64;++i){
  size_t bit=32;
  while(j&bit){j^=bit;bit>>=1;}
  j^=bit;
  if(i<j)std::swap(a[i],a[j]);
 }
 static const auto twiddles=[] {
  std::array<Complex,32> t{};
  for(size_t k=0;k<t.size();++k){const double ang=-2*PI*double(k)/64.0;t[k]={std::cos(ang),std::sin(ang)};}
  return t;
 }();
 for(size_t len=2;len<=64;len*=2){
  const size_t half=len/2,step=64/len;
  for(size_t base=0;base<64;base+=len){
   for(size_t k=0;k<half;++k){
    const Complex u=a[base+k],v=a[base+k+half]*twiddles[k*step];
    a[base+k]=u+v;a[base+k+half]=u-v;
   }
  }
 }
 double weighted=0,total=0;
 for(size_t k=0;k<64;++k){const double power=std::norm(a[k]);weighted+=k*power;total+=power;}
 return total>0?weighted/(64*total):0;
}
}

AnalysisResult DspAnalyzer::analyze(const Frame& f,bool light){
 require(f.iq.size()>=8&&f.iq.size()%8==0,"invalid IQ frame size");
 const size_t n=f.iq.size()/8;double power=0;
 for(size_t i=0;i<n;++i){double re=read_float_le(f.iq.data()+i*8),im=read_float_le(f.iq.data()+i*8+4);if(!std::isfinite(re)||!std::isfinite(im))throw std::runtime_error("nonfinite IQ sample");power+=re*re+im*im;require(std::isfinite(power),"IQ energy overflow");}
 AnalysisResult r;r.mean_power=power/n;
 if(light)return r;
 // The old algorithm used the first 64 IQ samples. Preserve its mathematical
 // definition exactly; odd / shorter frames keep the reference fallback.
 const size_t bins=std::min<size_t>(64,n);
 r.spectral_centroid=(bins==64&&!reference_dft_)?fft64_centroid(f.iq):reference_centroid(f.iq,bins);
 return r;
}
std::unique_ptr<IAnalyzer> make_analyzer(const std::string& dsp_backend){
 require(dsp_backend=="fft"||dsp_backend=="reference","--dsp-backend must be fft or reference");
 return std::make_unique<DspAnalyzer>(dsp_backend=="reference");
}
}
