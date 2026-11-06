/*
 * photo_info: what the photographs on this phone say of themselves, as numbers, for checking the camera's flash without taking a picture off the
 * phone: how many there are and, for the newest ones, the size, the EXIF flash tag, the exposure and the light in the picture.
 * No file names, and no pixels: the pictures may show the owner's room and stay where they are.
 *
 * usage: photo_info [NEWEST]    the NEWEST photographs (default 2), the newest first
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DCIM    "/var/mobile/Media/DCIM"
#define MOST    256

struct photo {
	char    path[256];
	time_t  when;
	off_t   size;
};
static struct photo photos[MOST];
static int count;

static int
newest_first(const void *a, const void *b)
{
	const struct photo *x = a, *y = b;

	return x->when < y->when ? 1 : x->when > y->when ? -1 : strcmp(y->path, x->path);
}

static void
collect(void)
{
	DIR *top = opendir(DCIM);
	struct dirent *d;

	while (top != NULL && (d = readdir(top)) != NULL) {
		char folder[128];
		DIR *dir;
		struct dirent *e;

		if (d->d_name[0] == '.' || (size_t)snprintf(folder, sizeof(folder), DCIM "/%s", d->d_name) >= sizeof(folder) || (dir = opendir(folder)) == NULL) {
			continue;
		}
		while ((e = readdir(dir)) != NULL && count < MOST) {
			const char *dot = strrchr(e->d_name, '.');
			struct stat st;

			if (dot == NULL || (strcasecmp(dot, ".heic") != 0 && strcasecmp(dot, ".jpg") != 0)) {
				continue;
			}
			snprintf(photos[count].path, sizeof(photos[count].path), "%s/%s", folder, e->d_name);
			if (stat(photos[count].path, &st) == 0) {
				photos[count].when = st.st_mtime;
				photos[count].size = st.st_size;
				count++;
			}
		}
		closedir(dir);
	}
	if (top != NULL) {
		closedir(top);
	}
}

static double
number_of(CFDictionaryRef dict, CFStringRef key, double otherwise)
{
	CFTypeRef value = dict != NULL ? CFDictionaryGetValue(dict, key) : NULL;
	double out = otherwise;

	if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) > 0) {
		value = CFArrayGetValueAtIndex(value, 0);
	}
	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberDoubleType, &out);
	}
	return out;
}

/* the light in a picture, from a small copy of it that is thrown away */
static void
light(CGImageSourceRef source, double *mean, double *bright, double *rgb_mean)
{
	enum { SIDE = 64 };
	static uint8_t pixels[SIDE * SIDE * 4];
	const void *keys[] = { kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceThumbnailMaxPixelSize, kCGImageSourceCreateThumbnailWithTransform };
	int side = SIDE;
	CFNumberRef max = CFNumberCreate(NULL, kCFNumberIntType, &side);
	const void *values[] = { kCFBooleanTrue, max, kCFBooleanTrue };
	CFDictionaryRef options = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CGImageRef small = CGImageSourceCreateThumbnailAtIndex(source, 0, options);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef context = CGBitmapContextCreate(pixels, SIDE, SIDE, 8, SIDE * 4, rgb, kCGImageAlphaPremultipliedLast);

	*mean = *bright = -1;
	if (small != NULL && context != NULL) {
		double sum = 0, channel[3] = { 0, 0, 0 };
		int over = 0;

		CGContextDrawImage(context, CGRectMake(0, 0, SIDE, SIDE), small);
		for (int i = 0; i < SIDE * SIDE; i++) {
			double y = 0.299 * pixels[i * 4] + 0.587 * pixels[i * 4 + 1] + 0.114 * pixels[i * 4 + 2];

			sum += y;
			over += y > 250;
			for (int c = 0; c < 3; c++) {
				channel[c] += pixels[i * 4 + c];
			}
		}
		for (int c = 0; c < 3; c++) {
			rgb_mean[c] = channel[c] / (SIDE * SIDE);
		}
		*mean = sum / (SIDE * SIDE);
		*bright = 100.0 * over / (SIDE * SIDE);
	}
	memset(pixels, 0, sizeof(pixels));
	if (context != NULL) {
		CGContextRelease(context);
	}
	CGColorSpaceRelease(rgb);
	if (small != NULL) {
		CGImageRelease(small);
	}
	CFRelease(options);
	CFRelease(max);
}

int
main(int argc, char **argv)
{
	int newest = argc > 1 ? atoi(argv[1]) : 2;

	collect();
	qsort(photos, (size_t)count, sizeof(photos[0]), newest_first);
	printf("photo_info: %d photographs\n", count);
	for (int i = 0; i < count && i < newest; i++) {
		CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)photos[i].path, (CFIndex)strlen(photos[i].path), false);
		CGImageSourceRef source = CGImageSourceCreateWithURL(url, NULL);
		CFDictionaryRef properties = source != NULL ? CGImageSourceCopyPropertiesAtIndex(source, 0, NULL) : NULL;
		CFDictionaryRef exif = properties != NULL ? CFDictionaryGetValue(properties, kCGImagePropertyExifDictionary) : NULL;
		double mean = -1, bright = -1, channel[3] = { -1, -1, -1 };

		if (source != NULL) {
			light(source, &mean, &bright, channel);
		}
		printf("photo_info: newest-%d: %.0f x %.0f, %lld KB, flash tag %#x, exposure %.4f s, ISO %.0f, mean light %.1f of 255 (R %.1f G %.1f B %.1f), %.1f %% over 250\n", i,
		    number_of(properties, kCGImagePropertyPixelWidth, 0), number_of(properties, kCGImagePropertyPixelHeight, 0), (long long)photos[i].size / 1024,
		    (unsigned)number_of(exif, kCGImagePropertyExifFlash, 0xffff), number_of(exif, kCGImagePropertyExifExposureTime, -1),
		    number_of(exif, kCGImagePropertyExifISOSpeedRatings, -1), mean, channel[0], channel[1], channel[2], bright);
		if (properties != NULL) {
			CFRelease(properties);
		}
		if (source != NULL) {
			CFRelease(source);
		}
		CFRelease(url);
	}
	return 0;
}
