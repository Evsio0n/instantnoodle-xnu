/*
 * vt_test: whether VideoToolbox turns and scales pictures here, and which way fails.
 *
 * Photos shows an HDR photo's gain map in place of the photo (backlog item 17, runs 231531-233935): its decoder gets
 * -12914 (kVTImageRotationNotSupportedErr) for the base picture at full size while thumbnails decode in colour. In
 * 20F66's VideoToolbox thirteen places give that error, all on the rotation path; VTImageRotationSessionCreate (what
 * the decoder uses) asks for kVTPixelRotationPropertyKey_EnableGPUAcceleratedTransfer, and there is a CPU path as well
 * (vt_Rotate_y420 -> vImageRotate90_Planar8). This asks VideoToolbox directly, one case a line: the picture's kind and
 * size, the turn, the GPU asked for or not, and the OSStatus; then the same through a VTPixelTransferSession (a scale);
 * then, with a file named, ImageIO's decode of it whole and with its orientation applied.
 *
 *   usage: vt_test [FILE...]
 *          vt_test --pixels [FILE...]    what comes out, not only the OSStatus: VideoToolbox's Metal path (M5e)
 *
 * --pixels fills a picture with a known pattern (red across, green down), transfers it with the GPU asked for, refused
 * and left to VideoToolbox, and says how far the result is from what it should be. A status of 0 over a black picture
 * is what a compute pass that writes nowhere would look like, and only the bytes can tell.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <ImageIO/ImageIO.h>
#include <VideoToolbox/VideoToolbox.h>
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t
now_ms(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return mach_absolute_time() * base.numer / base.denom / 1000000;
}

static CVPixelBufferRef
picture(OSType kind, size_t width, size_t height)
{
	const void *keys[] = { kCVPixelBufferIOSurfacePropertiesKey };
	const void *values[] = { CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	                         &kCFTypeDictionaryValueCallBacks) };
	CFDictionaryRef attributes = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CVPixelBufferRef buffer = NULL;
	CVReturn made = CVPixelBufferCreate(NULL, width, height, kind, attributes, &buffer);

	CFRelease(values[0]);
	CFRelease(attributes);
	if (made != kCVReturnSuccess) {
		printf("vt_test: no %.4s %zux%zu picture (%d)\n", (const char *)&(uint32_t){ __builtin_bswap32(kind) }, width,
		    height, made);
		return NULL;
	}
	return buffer;
}

static CFStringRef
spi_key(const char *name)
{
	CFStringRef *key = dlsym(RTLD_DEFAULT, name);

	return key != NULL ? *key : NULL;
}

/* one turn of one kind of picture, the GPU asked for (1), refused (0) or left to VideoToolbox (-1) */
static void
turn(OSType kind, size_t width, size_t height, int degrees, int gpu)
{
	static const struct { int degrees; const CFStringRef *value; } turns[] = {
		{ 0, &kVTRotation_0 }, { 90, &kVTRotation_CW90 }, { 180, &kVTRotation_180 }, { 270, &kVTRotation_CCW90 },
	};
	CFStringRef gpu_key = spi_key("kVTPixelRotationPropertyKey_EnableGPUAcceleratedTransfer");
	VTPixelRotationSessionRef session = NULL;
	CVPixelBufferRef from = picture(kind, width, height), to = NULL;
	OSStatus status;
	uint64_t began;

	if (from == NULL) {
		return;
	}
	to = degrees % 180 != 0 ? picture(kind, height, width) : picture(kind, width, height);
	if (to == NULL) {
		CVPixelBufferRelease(from);
		return;
	}
	status = VTPixelRotationSessionCreate(NULL, &session);
	if (status == noErr) {
		for (unsigned i = 0; i < sizeof(turns) / sizeof(turns[0]); i++) {
			if (turns[i].degrees == degrees) {
				status = VTSessionSetProperty(session, kVTPixelRotationPropertyKey_Rotation, *turns[i].value);
			}
		}
	}
	if (status == noErr && gpu >= 0 && gpu_key != NULL) {
		status = VTSessionSetProperty(session, gpu_key, gpu ? kCFBooleanTrue : kCFBooleanFalse);
		if (status != noErr) {
			printf("vt_test:   (the GPU property was refused: %d)\n", (int)status);
			status = noErr;
		}
	}
	began = now_ms();
	if (status == noErr) {
		status = VTPixelRotationSessionRotateImage(session, from, to);
	}
	printf("vt_test: turn %.4s %zux%zu by %d, GPU %s: %d (%llu ms)\n",
	    (const char *)&(uint32_t){ __builtin_bswap32(kind) }, width, height, degrees,
	    gpu < 0 ? "as VideoToolbox likes" : gpu ? "asked for" : "refused", (int)status,
	    (unsigned long long)(now_ms() - began));
	if (session != NULL) {
		VTPixelRotationSessionInvalidate(session);
		CFRelease(session);
	}
	CVPixelBufferRelease(from);
	CVPixelBufferRelease(to);
}

/* a scale to the screen's width, as a decoder making a picture to show would */
static void
scale(OSType kind, size_t width, size_t height, OSType into, int gpu)
{
	CFStringRef gpu_key = spi_key("kVTPixelTransferPropertyKey_EnableGPUAcceleratedTransfer");
	VTPixelTransferSessionRef session = NULL;
	size_t to_width = 1125, to_height = height * 1125 / width & ~(size_t)1;
	CVPixelBufferRef from = picture(kind, width, height), to = picture(into, to_width, to_height);
	OSStatus status = from != NULL && to != NULL ? VTPixelTransferSessionCreate(NULL, &session) : -1;
	uint64_t began;

	if (status == noErr && gpu >= 0 && gpu_key != NULL) {
		VTSessionSetProperty(session, gpu_key, gpu ? kCFBooleanTrue : kCFBooleanFalse);
	}
	began = now_ms();
	if (status == noErr) {
		status = VTPixelTransferSessionTransferImage(session, from, to);
	}
	printf("vt_test: scale %.4s %zux%zu to %.4s %zux%zu, GPU %s: %d (%llu ms)\n",
	    (const char *)&(uint32_t){ __builtin_bswap32(kind) }, width, height,
	    (const char *)&(uint32_t){ __builtin_bswap32(into) }, to_width, to_height,
	    gpu < 0 ? "as VideoToolbox likes" : gpu ? "asked for" : "refused", (int)status,
	    (unsigned long long)(now_ms() - began));
	if (session != NULL) {
		VTPixelTransferSessionInvalidate(session);
		CFRelease(session);
	}
	if (from != NULL) {
		CVPixelBufferRelease(from);
	}
	if (to != NULL) {
		CVPixelBufferRelease(to);
	}
}

/* ImageIO's decode of a file: whole, and at most 4096 on a side with its orientation applied (says "file N" only) */
static void
decode(const char *path, unsigned number)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CGImageSourceRef source = url != NULL ? CGImageSourceCreateWithURL(url, NULL) : NULL;
	uint64_t began;
	CGImageRef image;

	if (source == NULL) {
		printf("vt_test: file %u cannot be opened\n", number);
		if (url != NULL) {
			CFRelease(url);
		}
		return;
	}
	began = now_ms();
	image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
	printf("vt_test: file %u decoded whole: %s %zux%zu (%llu ms)\n", number, image != NULL ? "yes" : "NO",
	    image != NULL ? CGImageGetWidth(image) : 0, image != NULL ? CGImageGetHeight(image) : 0,
	    (unsigned long long)(now_ms() - began));
	if (image != NULL) {
		CGImageRelease(image);
	}
	{
		int side = 4096;
		CFNumberRef most = CFNumberCreate(NULL, kCFNumberIntType, &side);
		const void *keys[] = { kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
		                       kCGImageSourceThumbnailMaxPixelSize };
		const void *values[] = { kCFBooleanTrue, kCFBooleanTrue, most };
		CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);

		began = now_ms();
		image = CGImageSourceCreateThumbnailAtIndex(source, 0, options);
		printf("vt_test: file %u decoded turned, up to %d: %s %zux%zu (%llu ms)\n", number, side,
		    image != NULL ? "yes" : "NO", image != NULL ? CGImageGetWidth(image) : 0,
		    image != NULL ? CGImageGetHeight(image) : 0, (unsigned long long)(now_ms() - began));
		if (image != NULL) {
			CGImageRelease(image);
		}
		CFRelease(options);
		CFRelease(most);
	}
	CFRelease(source);
	CFRelease(url);
}

/* The pattern: BGRA, red across, green down, blue 128; 420f, grey across, or one colour when `y` is given. */
static void
fill(CVPixelBufferRef buffer, int y, int cb, int cr)
{
	const size_t width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer);

	CVPixelBufferLockBaseAddress(buffer, 0);
	if (CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA) {
		uint8_t *base = CVPixelBufferGetBaseAddress(buffer);
		const size_t row = CVPixelBufferGetBytesPerRow(buffer);

		for (size_t j = 0; j < height; j++) {
			for (size_t i = 0; i < width; i++) {
				uint8_t *p = base + j * row + i * 4;

				p[0] = 128;
				p[1] = (uint8_t)(255 * j / (height - 1));
				p[2] = (uint8_t)(255 * i / (width - 1));
				p[3] = 255;
			}
		}
	} else {
		uint8_t *luma = CVPixelBufferGetBaseAddressOfPlane(buffer, 0), *chroma = CVPixelBufferGetBaseAddressOfPlane(buffer, 1);
		const size_t lrow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0), crow = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);

		for (size_t j = 0; j < height; j++) {
			for (size_t i = 0; i < width; i++) {
				luma[j * lrow + i] = y >= 0 ? (uint8_t)y : (uint8_t)(255 * i / (width - 1));
			}
		}
		for (size_t j = 0; j < height / 2; j++) {
			for (size_t i = 0; i < width / 2; i++) {
				chroma[j * crow + i * 2] = y >= 0 ? (uint8_t)cb : 128;
				chroma[j * crow + i * 2 + 1] = y >= 0 ? (uint8_t)cr : 128;
			}
		}
	}
	CVPixelBufferUnlockBaseAddress(buffer, 0);
}

/* How far a BGRA result is from `want` (blue, green, red; negative: the BGRA pattern, or grey across for a grey source). */
static void
judge(CVPixelBufferRef buffer, bool grey, const int *want, char *out, size_t size)
{
	const size_t width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer);
	unsigned zero = 0, worst = 0;

	CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
	const uint8_t *base = CVPixelBufferGetBaseAddress(buffer);
	const size_t row = CVPixelBufferGetBytesPerRow(buffer);

	for (unsigned j = 0; j < 16; j++) {
		for (unsigned i = 0; i < 16; i++) {
			const size_t x = (width - 1) * i / 15, y = (height - 1) * j / 15;
			const uint8_t *p = base + y * row + x * 4;
			int expect[3];

			if (want != NULL) {
				memcpy(expect, want, sizeof(expect));
			} else if (grey) {
				expect[0] = expect[1] = expect[2] = (int)(255 * x / (width - 1));
			} else {
				expect[0] = 128;
				expect[1] = (int)(255 * y / (height - 1));
				expect[2] = (int)(255 * x / (width - 1));
			}
			zero += p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
			for (unsigned k = 0; k < 3; k++) {
				const unsigned off = (unsigned)abs((int)p[k] - expect[k]);

				worst = off > worst ? off : worst;
			}
		}
	}

	const uint8_t *m = base + (height / 2) * row + (width / 2) * 4;

	snprintf(out, size, "worst off by %u, %u of 256 all zero, middle BGRA %u %u %u %u", worst, zero, m[0], m[1], m[2], m[3]);
	CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
}

static void
pixels(OSType kind, size_t width, size_t height, size_t to_width, size_t to_height, int y, int cb, int cr, const int *want, int gpu)
{
	CFStringRef gpu_key = spi_key("kVTPixelTransferPropertyKey_EnableGPUAcceleratedTransfer");
	VTPixelTransferSessionRef session = NULL;
	CVPixelBufferRef from = picture(kind, width, height), to = picture(kCVPixelFormatType_32BGRA, to_width, to_height);
	OSStatus status = from != NULL && to != NULL ? VTPixelTransferSessionCreate(NULL, &session) : -1;
	char verdict[120] = "not looked at";
	uint64_t began;

	if (from != NULL) {
		fill(from, y, cb, cr);
	}
	/* 0xAA left behind says the transfer wrote nothing; zeros say it wrote zeros */
	if (to != NULL) {
		CVPixelBufferLockBaseAddress(to, 0);
		memset(CVPixelBufferGetBaseAddress(to), 0xAA, CVPixelBufferGetBytesPerRow(to) * to_height);
		CVPixelBufferUnlockBaseAddress(to, 0);
	}
	if (status == noErr && gpu >= 0 && gpu_key != NULL) {
		VTSessionSetProperty(session, gpu_key, gpu ? kCFBooleanTrue : kCFBooleanFalse);
	}
	began = now_ms();
	if (status == noErr) {
		status = VTPixelTransferSessionTransferImage(session, from, to);
	}
	const uint64_t took = now_ms() - began;

	if (status == noErr) {
		judge(to, kind != kCVPixelFormatType_32BGRA, want, verdict, sizeof(verdict));
	}
	printf("vt_test: pixels %.4s %zux%zu to BGRA %zux%zu, GPU %s: %d (%llu ms): %s\n",
	    (const char *)&(uint32_t){ __builtin_bswap32(kind) }, width, height, to_width, to_height,
	    gpu < 0 ? "as VideoToolbox likes" : gpu ? "asked for" : "refused", (int)status, (unsigned long long)took, verdict);
	if (session != NULL) {
		VTPixelTransferSessionInvalidate(session);
		CFRelease(session);
	}
	if (from != NULL) {
		CVPixelBufferRelease(from);
	}
	if (to != NULL) {
		CVPixelBufferRelease(to);
	}
}

/* And what ImageIO makes of a file: its picture drawn small, a few pixels of it; a generic test file (numbers only). */
static void
decode_pixels(const char *path, unsigned number)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	CGImageSourceRef source = url != NULL ? CGImageSourceCreateWithURL(url, NULL) : NULL;
	CGImageRef image = source != NULL ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
	uint8_t small[16 * 16 * 4] = { 0 };

	if (image == NULL) {
		printf("vt_test: file %u: no picture\n", number);
	} else {
		CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
		CGContextRef context = CGBitmapContextCreate(small, 16, 16, 8, 64, rgb,
		    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
		uint64_t began = now_ms();

		CGContextDrawImage(context, CGRectMake(0, 0, 16, 16), image);
		printf("vt_test: file %u is %zux%zu; drawn in %llu ms; BGRA at the corners and middle: %u %u %u, %u %u %u, %u %u %u\n",
		    number, CGImageGetWidth(image), CGImageGetHeight(image), (unsigned long long)(now_ms() - began),
		    small[0], small[1], small[2], small[15 * 64 + 15 * 4], small[15 * 64 + 15 * 4 + 1], small[15 * 64 + 15 * 4 + 2],
		    small[8 * 64 + 8 * 4], small[8 * 64 + 8 * 4 + 1], small[8 * 64 + 8 * 4 + 2]);
		CGContextRelease(context);
		CGColorSpaceRelease(rgb);
		CGImageRelease(image);
	}
	if (source != NULL) {
		CFRelease(source);
	}
	if (url != NULL) {
		CFRelease(url);
	}
}

static int
pixels_all(int argc, char **argv)
{
	static const int head[3] = { 128, 133, 42 };   /* (Y 105, Cb 141, Cr 83: real Metal and this Mac's decoder, TODO M5d) */

	for (int i = 2; i < argc; i++) {
		decode(argv[i], (unsigned)i - 1);
		decode_pixels(argv[i], (unsigned)i - 1);
	}
	for (int gpu = -1; gpu <= 1; gpu++) {
		pixels(kCVPixelFormatType_32BGRA, 1125, 2436, 787, 1705, -1, 0, 0, NULL, gpu);
		pixels(kCVPixelFormatType_32BGRA, 1125, 2436, 1125, 2436, -1, 0, 0, NULL, gpu);
		pixels(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 1104, 1160, 1104, 1160, -1, 0, 0, NULL, gpu);
		pixels(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 1104, 1160, 552, 580, -1, 0, 0, NULL, gpu);
		pixels(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 1104, 1160, 552, 580, 105, 141, 83, head, gpu);
	}
	printf("vt_test: done\n");
	return 0;
}

int
main(int argc, char **argv)
{
	static const struct { OSType kind; size_t width, height; } pictures[] = {
		{ kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 512, 384 },
		{ kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 4032, 3024 },
		{ kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 5712, 4284 },
		{ kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, 4032, 3024 },
		{ kCVPixelFormatType_32BGRA, 4032, 3024 },
		{ kCVPixelFormatType_OneComponent8, 2016, 1512 },
	};

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("vt_test: the GPU properties are %s (rotation), %s (transfer)\n",
	    spi_key("kVTPixelRotationPropertyKey_EnableGPUAcceleratedTransfer") != NULL ? "there" : "missing",
	    spi_key("kVTPixelTransferPropertyKey_EnableGPUAcceleratedTransfer") != NULL ? "there" : "missing");
	if (argc > 1 && strcmp(argv[1], "--pixels") == 0) {
		return pixels_all(argc, argv);
	}
	for (unsigned i = 0; i < sizeof(pictures) / sizeof(pictures[0]); i++) {
		for (int gpu = -1; gpu <= 1; gpu++) {
			turn(pictures[i].kind, pictures[i].width, pictures[i].height, 0, gpu);
			turn(pictures[i].kind, pictures[i].width, pictures[i].height, 90, gpu);
		}
	}
	for (int gpu = -1; gpu <= 1; gpu++) {
		scale(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 4032, 3024, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
		    gpu);
		scale(kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 4032, 3024, kCVPixelFormatType_32BGRA, gpu);
	}
	for (int i = 1; i < argc; i++) {
		decode(argv[i], (unsigned)i);
	}
	printf("vt_test: done\n");
	return 0;
}
