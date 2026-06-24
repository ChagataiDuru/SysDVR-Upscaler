#include "render/FidelityFxConstants.h"
#define A_CPU 1
#include <ffx_a.h>
#include <ffx_fsr1.h>
#include <ffx_cas.h>
namespace ns60 {
FsrEasuConstants makeFsrEasuConstants(float iw,float ih,float ow,float oh) noexcept { FsrEasuConstants c;FsrEasuCon(c.con0.data(),c.con1.data(),c.con2.data(),c.con3.data(),iw,ih,iw,ih,ow,oh);return c; }
std::array<std::uint32_t,4> makeFsrRcasConstants(float strength) noexcept { std::array<std::uint32_t,4> c{};const float stops=2.0F*(1.0F-strength);FsrRcasCon(c.data(),stops);return c; }
SharpenConstants makeCasConstants(float strength,float width,float height) noexcept { SharpenConstants c;CasSetup(c.con0.data(),c.con1.data(),strength,width,height,width,height);return c; }
} // namespace ns60
