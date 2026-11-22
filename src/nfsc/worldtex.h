#ifndef NFSC_WORLDTEX_H_
#define NFSC_WORLDTEX_H_

// World textures of the track sections at reduced resolution: their top mip levels are left out when the files are
// read (worldtex.cpp).

#include <x86.h>

namespace nfsc { namespace worldtex {

// Main thread, before guest code runs: installs the file read filters and starts building the sections' layouts.
void install();

// Around the creation of a texture (TextureInfo address): a texture of a reduced section gets the game's mip skip
// lowered by the levels already left out of it, so the created texture is the one the game's setting asks for.
class CreationSkip
{
public:
    explicit CreationSkip(x86::reg32 info);
    ~CreationSkip();

private:
    CreationSkip(const CreationSkip&) = delete;
    CreationSkip& operator=(const CreationSkip&) = delete;
    x86::reg32 m_saved;
    bool       m_active;
};

}}

#endif /* !NFSC_WORLDTEX_H_ */
