#ifndef ABR_ETC2_DXT5_H
#define ABR_ETC2_DXT5_H

#include <stddef.h>
#include <stdint.h>

/* Transcode one ETC2 RGBA8/EAC mip into BC3/DXT5.  Both formats use one
 * 16-byte block per 4x4 texels, so the returned byte count normally equals
 * the input byte count.  The caller owns the returned buffer. */
uint8_t *abr_etc2_rgba8_to_dxt5(const void *src, size_t src_size,
                                unsigned width, unsigned height,
                                int srgb, size_t *dst_size);

#endif
