// Single translation unit for miniaudio implementation.
// Only ONE .cpp file in the project should define this.

// stb_vorbis must be included BEFORE miniaudio to enable OGG Vorbis decoding.
// miniaudio checks for STB_VORBIS_INCLUDE_STB_VORBIS_H to know if vorbis is available.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

// Now include the full stb_vorbis implementation
#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
