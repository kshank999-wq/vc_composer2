/* TinySoundFont (MIT, Bernhard Schelling) with stb_vorbis (public domain,
   Sean Barrett) for compressed .sf3 banks: the implementation, compiled once
   as C. VC Composer's copy of tsf.h keeps samples 16-bit (see its header). */
#include "stb_vorbis.c"
#define TSF_IMPLEMENTATION
#include "tsf.h"
