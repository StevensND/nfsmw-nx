// nfsmw - deko3d build: posix_memalign, which FFmpeg (libavutil's av_malloc) calls. devkitPro's newlib declares it
// in stdlib.h without defining it; in the NVK build the Mesa SDK's static libraries bring one. Blocks from newlib's
// memalign are released with free(), as POSIX requires of posix_memalign's.

#include <malloc.h>

#include <cerrno>
#include <cstddef>

extern "C" int posix_memalign(void** resultado, size_t alineacion, size_t bytes) {
  if (alineacion < sizeof(void*) || (alineacion & (alineacion - 1)) != 0) {
    return EINVAL;
  }
  void* bloque = memalign(alineacion, bytes);
  if (!bloque && bytes) {
    return ENOMEM;
  }
  *resultado = bloque;
  return 0;
}
