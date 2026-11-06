/*
 * ci_gain: an HDR photo's gain map through CoreImage, by its bytes (K33).
 *
 * Photos makes the gain map's image in +[PFImageMetadata createImageRefFromAuxiliaryImagePixelBuffer:applyingOrientation:scaleFactor:]:
 * a CIImage over the decoder's L008 pixel buffer, turned, and -[CIContext createCGImage:fromRect:]. On the phone that image comes
 * out with nothing in it (runs 031334, 031904), and with no gain there are no highlights. This asks for the same thing over a
 * known pattern, grey across, with no photo in it, and says what comes out; the same source builds on a Mac, which is real Metal.
 *
 *   usage: ci_gain
 */
#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hk_gain_image.h"

static CVPixelBufferRef
gain_map(size_t width, size_t height)
{
	NSDictionary *attributes = @{ (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
	CVPixelBufferRef buffer = NULL;

	if (CVPixelBufferCreate(NULL, width, height, kCVPixelFormatType_OneComponent8, (__bridge CFDictionaryRef)attributes, &buffer) != kCVReturnSuccess) {
		printf("ci_gain: no L008 pixel buffer\n");
		return NULL;
	}
	CVPixelBufferLockBaseAddress(buffer, 0);
	uint8_t *base = CVPixelBufferGetBaseAddress(buffer);
	const size_t row = CVPixelBufferGetBytesPerRow(buffer);

	for (size_t y = 0; y < height; y++) {
		for (size_t x = 0; x < width; x++) {
			base[y * row + x] = (uint8_t)(64 * x / (width - 1) + 191 * y / (height - 1));   /* (across and down apart: a turn shows) */
		}
	}
	CVPixelBufferUnlockBaseAddress(buffer, 0);
	return buffer;
}

static void
look(const char *what, CGImageRef image)
{
	if (image == NULL) {
		printf("ci_gain: %s: no image\n", what);
		return;
	}
	const size_t width = CGImageGetWidth(image), height = CGImageGetHeight(image), row = CGImageGetBytesPerRow(image), bits = CGImageGetBitsPerPixel(image);
	CFDataRef bytes = CGDataProviderCopyData(CGImageGetDataProvider(image));
	const uint8_t *data = bytes != NULL ? CFDataGetBytePtr(bytes) : NULL;
	const size_t stride = bits / 8;
	unsigned nonzero = 0, top[4] = { 0 };

	for (size_t j = 0; data != NULL && j < 32; j++) {
		for (size_t i = 0; i < 32; i++) {
			const uint8_t *p = data + (height - 1) * j / 31 * row + (width - 1) * i / 31 * stride;
			unsigned any = 0;

			for (size_t k = 0; k < stride && k < 4; k++) {
				top[k] = p[k] > top[k] ? p[k] : top[k];
				any |= p[k];
			}
			nonzero += any != 0;
		}
	}
	printf("ci_gain: %s: %zux%zu, %zu bits, row %zu, alpha %d, bitmap %#x; of 1024 points %u not zero, highest by byte %u %u %u %u", what, width, height, bits, row,
	    (int)CGImageGetAlphaInfo(image), (unsigned)CGImageGetBitmapInfo(image), nonzero, top[0], top[1], top[2], top[3]);
	for (unsigned f = 0; data != NULL && f < 5; f++) {
		const uint8_t *p = data + (height / 2) * row + (width - 1) * f / 4 * stride;
		const uint8_t *q = data + (height - 1) * f / 4 * row + (width / 2) * stride;

		printf("%s %u/%u", f == 0 ? "; across then down" : ",", p[0], q[0]);
	}
	CFStringRef space = CGImageGetColorSpace(image) != NULL ? CGColorSpaceCopyName(CGImageGetColorSpace(image)) : NULL;

	printf("; space %s\n", space != NULL ? CFStringGetCStringPtr(space, kCFStringEncodingUTF8) : "(no name)");
	if (bytes != NULL) {
		CFRelease(bytes);
	}
}

int
main(void)
{
	@autoreleasepool {
		static const struct { size_t width, height; CGImagePropertyOrientation turn; const char *name; } cases[] = {
			{ 128, 128, kCGImagePropertyOrientationUp, "128x128 upright" },
			{ 240, 180, kCGImagePropertyOrientationUp, "240x180 upright" },
			{ 256, 256, kCGImagePropertyOrientationUp, "256x256 upright" },
			{ 320, 240, kCGImagePropertyOrientationUp, "320x240 upright" },
			{ 512, 512, kCGImagePropertyOrientationUp, "512x512 upright" },
			{ 1024, 768, kCGImagePropertyOrientationUp, "1024x768 upright" },
			{ 2016, 1512, kCGImagePropertyOrientationUp, "2016x1512 upright" },
			{ 2016, 1512, kCGImagePropertyOrientationRight, "2016x1512 turned as Photos does" },
		};
		CIContext *plain = [CIContext context];
		CIContext *linear = [CIContext contextWithOptions:@{ kCIContextWorkingColorSpace: (id)[NSNull null] }];
		CIContext *software = [CIContext contextWithOptions:@{ kCIContextUseSoftwareRenderer: @YES }];

		printf("ci_gain: context %s\n", [[[plain class] description] UTF8String]);
		for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
			CVPixelBufferRef buffer = gain_map(cases[c].width, cases[c].height);

			if (buffer == NULL) {
				continue;
			}
			CIImage *source = [CIImage imageWithCVPixelBuffer:buffer];
			CIImage *turned = [source imageByApplyingCGOrientation:cases[c].turn];
			char what[128];

			snprintf(what, sizeof(what), "%s, default context", cases[c].name);
			CGImageRef image = [plain createCGImage:turned fromRect:[turned extent]];
			look(what, image);
			CGImageRelease(image);
			snprintf(what, sizeof(what), "%s, no colour management", cases[c].name);
			image = [linear createCGImage:turned fromRect:[turned extent]];
			look(what, image);
			CGImageRelease(image);
			snprintf(what, sizeof(what), "%s, software renderer", cases[c].name);
			image = [software createCGImage:turned fromRect:[turned extent]];
			look(what, image);
			CGImageRelease(image);
			CVPixelBufferRelease(buffer);
		}
		/* the CPU's copy against CoreImage's, each orientation, whole: on a Mac this is real Metal; on the phone, where CoreImage is empty, it says so */
		for (unsigned turn = 1; turn <= 8; turn++) {
			static const double scales[] = { 1.0, 0.5, 0.75 };

			for (unsigned k = 0; k < 3; k++) {
				CVPixelBufferRef buffer = gain_map(504, 378);
				CIImage *image = [[CIImage imageWithCVPixelBuffer:buffer] imageByApplyingCGOrientation:(CGImagePropertyOrientation)turn];

				if (scales[k] != 1.0) {
					image = [image imageByApplyingTransform:CGAffineTransformMakeScale(scales[k], scales[k])];
				}
				CGImageRef theirs = [plain createCGImage:image fromRect:[image extent]], ours = hk_gain_image(buffer, turn, scales[k]);
				const size_t tw = theirs != NULL ? CGImageGetWidth(theirs) : 0, th = theirs != NULL ? CGImageGetHeight(theirs) : 0;
				const size_t ow = ours != NULL ? CGImageGetWidth(ours) : 0, oh = ours != NULL ? CGImageGetHeight(ours) : 0;
				unsigned worst = 0, off = 0;

				if (tw == ow && th == oh && tw != 0) {
					CFDataRef a = CGDataProviderCopyData(CGImageGetDataProvider(theirs)), b = CGDataProviderCopyData(CGImageGetDataProvider(ours));
					const uint8_t *pa = CFDataGetBytePtr(a), *pb = CFDataGetBytePtr(b);

					for (size_t y = 0; y < th; y++) {
						for (size_t x = 0; x < tw; x++) {
							const int d = abs((int)pa[y * CGImageGetBytesPerRow(theirs) + x * 4] - (int)pb[y * CGImageGetBytesPerRow(ours) + x * 4]);

							worst = (unsigned)d > worst ? (unsigned)d : worst;
							off += d > 1;
						}
					}
					CFRelease(a);
					CFRelease(b);
				}
				printf("ci_gain: orientation %u scale %.2f: CoreImage %zux%zu, this %zux%zu, worst off by %u, %u pixels off by more than 1\n", turn, scales[k], tw, th, ow, oh, worst, off);
				CGImageRelease(theirs);
				CGImageRelease(ours);
				CVPixelBufferRelease(buffer);
			}
		}
		printf("ci_gain: done\n");
	}
	return 0;
}
