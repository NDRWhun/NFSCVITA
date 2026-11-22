// The mixer rate is set twice: the DAC set-up 0x8a8396 stores the float at 0xa03990 (also the reverb's rate cap in
// 0x89a287), then RWAudioCoreWrapper::Initialize 0x5177f0 sends the integer at 0xa59a70 as the DAC's rate command.

#include "mixrate.h"

#include <platform/config.h>
#include <recomp.h>

namespace nfsc { namespace mixrate {

namespace
{

constexpr x86::reg32 SetupRate = 0xa03990;
constexpr x86::reg32 CommandRate = 0xa59a70;
constexpr int DefaultRate = 24000;
// Rates the DAC offers (0xa04240).
constexpr int Rates[] = { 22050, 24000, 32000, 44100, 48000 };

}

void install()
{
    const int wanted = pf::Config::getInt("mix_rate", DefaultRate);
    int rate = DefaultRate;
    for (int r : Rates)
    {
        if (r == wanted)
            rate = r;
    }
    rc::stf32(SetupRate, float(rate));
    rc::st32(CommandRate, x86::reg32(rate));
}

}}
