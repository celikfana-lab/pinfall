/* media.h - PinFall ffmpeg-subprocess video decoder (v8), internal header.
 *
 * The public API is declared in include/pc.h (PcVideo / pc_video_*).  This
 * header exposes one extra diagnostic used by tools/dump: the last read/open
 * error string (includes the ffmpeg stderr tail when the subprocess fails).
 *
 */
#ifndef PINFALL_MEDIA_H
#define PINFALL_MEDIA_H

#include "pc.h"

/* Last error recorded on this decoder (never NULL; "" if none). */
const char *pc_video_error(PcVideo *v);

#endif /* PINFALL_MEDIA_H */
