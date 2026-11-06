/*
 * An L008 gain map as the 32-bit image Photos wants, on the CPU: what CoreImage makes of it in
 * +[PFImageMetadata createImageRefFromAuxiliaryImagePixelBuffer:applyingOrientation:scaleFactor:], grey into R, G and B, measured
 * against CoreImage on a Mac's real Metal by ci_gain. Shared by libhackios-gainmap.dylib and that test.
 */
#ifndef HK_GAIN_IMAGE_H
#define HK_GAIN_IMAGE_H

#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CoreVideo.h>
#include <math.h>
#include <stdlib.h>

static void
hk_gain_image_free(void *info, const void *data, size_t size)
{
	(void)info;
	(void)size;
	free((void *)data);
}

/* Orientations are CGImagePropertyOrientation's. Rows go down; a coordinate is an edge, not a centre. */
static inline void
hk_gain_image_source(unsigned orientation, double u, double v, double w, double h, double *x, double *y)
{
	switch (orientation) {
	case 2: *x = w - u; *y = v; break;
	case 3: *x = w - u; *y = h - v; break;
	case 4: *x = u; *y = h - v; break;
	case 5: *x = v; *y = u; break;
	case 6: *x = v; *y = h - u; break;
	case 7: *x = w - v; *y = h - u; break;
	case 8: *x = w - v; *y = u; break;
	default: *x = u; *y = v; break;
	}
}

static inline CGImageRef
hk_gain_image(CVPixelBufferRef buffer, unsigned orientation, double scale)
{
	if (buffer == NULL || CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_OneComponent8 || scale <= 0) {
		return NULL;
	}
	const size_t width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer);
	const bool sideways = orientation >= 5 && orientation <= 8;
	const size_t out_width = (size_t)ceil((double)(sideways ? height : width) * scale - 1e-9);
	const size_t out_height = (size_t)ceil((double)(sideways ? width : height) * scale - 1e-9);
	uint8_t *out = out_width != 0 && out_height != 0 ? malloc(out_width * out_height * 4) : NULL;

	if (out == NULL) {
		return NULL;
	}
	CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
	const uint8_t *base = CVPixelBufferGetBaseAddress(buffer);
	const size_t row = CVPixelBufferGetBytesPerRow(buffer);

	for (size_t j = 0; j < out_height; j++) {
		uint8_t *to = out + j * out_width * 4;

		for (size_t i = 0; i < out_width; i++, to += 4) {
			double x, y;

			hk_gain_image_source(orientation, ((double)i + 0.5) / scale, ((double)j + 0.5) / scale, (double)width, (double)height, &x, &y);
			/* between the four nearest centres, clamped at the edge */
			const double fx = x - 0.5, fy = y - 0.5;
			const long x0 = (long)floor(fx), y0 = (long)floor(fy);
			const double ax = fx - (double)x0, ay = fy - (double)y0;
			const size_t l = x0 < 0 ? 0 : (size_t)x0 >= width ? width - 1 : (size_t)x0;
			const size_t r = x0 + 1 < 0 ? 0 : (size_t)(x0 + 1) >= width ? width - 1 : (size_t)(x0 + 1);
			const size_t t = y0 < 0 ? 0 : (size_t)y0 >= height ? height - 1 : (size_t)y0;
			const size_t b = y0 + 1 < 0 ? 0 : (size_t)(y0 + 1) >= height ? height - 1 : (size_t)(y0 + 1);
			const double top = base[t * row + l] * (1 - ax) + base[t * row + r] * ax;
			const double bottom = base[b * row + l] * (1 - ax) + base[b * row + r] * ax;
			const uint8_t value = (uint8_t)(top * (1 - ay) + bottom * ay + 0.5);

			to[0] = value;
			to[1] = value;
			to[2] = value;
			to[3] = 255;
		}
	}
	CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);

	CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, out, out_width * out_height * 4, hk_gain_image_free);
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGImageRef image = CGImageCreate(out_width, out_height, 8, 32, out_width * 4, space, (CGBitmapInfo)kCGImageAlphaNoneSkipLast, provider, NULL, false, kCGRenderingIntentDefault);

	CGColorSpaceRelease(space);
	CGDataProviderRelease(provider);
	return image;
}

#endif
