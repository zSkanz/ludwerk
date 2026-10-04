// The Ogg Vorbis decoder (D546): stb_vorbis, which miniaudio ships beside itself
// (`extras/stb_vorbis.c`) and decodes Vorbis through when it is there.
//
// **In a translation unit of its own, and nothing else in it.** miniaudio finds
// the decoder by its declarations, which `audio.cpp` includes ahead of
// miniaudio's own implementation; the definitions have to be compiled once,
// somewhere. Not at the end of `audio.cpp`, where miniaudio's notes put them
// for a program that is one file: the decoder leaves macros behind it -- `L`,
// `C` and `R` among them -- and the mixer's own code comes after.
//
// It was not compiled at all until the first real `.ogg` was played. The
// stream's length had been read from its pages by hand since D094, with a test
// made of a header and no audio, so `TimeLength` was right and every Vorbis
// file played the placeholder tone.
//
// The two warnings named below are ones MSVC raises while it generates code,
// after the switch that keeps a third party's headers quiet has had its say:
// "possibly uninitialised" in the decoder's seek, which it initialises on
// every path that reads it.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(disable : 4701 4703)
#endif
#include <extras/stb_vorbis.c>
