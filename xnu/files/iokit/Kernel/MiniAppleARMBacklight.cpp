/*
 * AppleARMBacklight: the service iOS's brightness code (CoreBrightness in backboardd, UserEventAgent) looks for. It is
 * found by class (IOServiceMatching) or by the name "backlight". What it asks of it is written down (every property set,
 * every user client opened, every call) so that the way from the brightness setting to the panel can be built on what is
 * really asked: bsd/dev -> MiniIOMobileFramebuffer.cpp (hk_display_set_backlight).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>

#include "MiniDrivers.h"

extern "C" void hk_display_set_backlight(uint32_t level);
extern "C" void hk_display_set_nits(uint32_t nits16, uint32_t cap16);
extern "C" void loader_log_line(const char *line);

/*
 * HDR (backlog item 17, 2026-11-04), boot-arg edr=1 (default 0: everything as before). What iOS 16.5's CoreBrightness
 * reads off this service to offer EDR, as found in 20F66 (read-only, the camera/backlog session's notes in TODO T38
 * (17)): new-backlight-architecture (kOSBooleanTrue, nothing else counts); backlight-calibration-parameters, a dictionary
 * of nine numbers, each its value times 65536, every one of them required (a missing one is a NULL read in backboardd);
 * user-accessible-max-nits, 4 bytes, 16.16 little-endian (a number is ignored); backlight-marketing-table, 16.16
 * little-endian nits, the slider's curve on the hardware's scale (without it CoreBrightness has no display at all).
 * HardwareAccessibleMaxNits = lMaxPanel, ExtrabrightEDRSupported = lMaxPanel > the user's maximum. The brightness then
 * comes as brightness-nits (nits x 65536) and brightness-nits-cap, not as backlight-level.
 * The panel's numbers: 430 nits at 0x7ff (its tree's qcom,mdss-dsi-panel-peak-brightness 4300000), its high brightness
 * mode 600 nits (a placeholder: not measured; hk_hbm.c's limits). The currents are placeholders CoreBrightness only
 * reports. Brightness below 2 nits, the table's first point, is not asked for.
 */
static TUNABLE(uint32_t, gEdr, "edr", 0);
#define NITS(n)                 ((unsigned long long)(n) << 16)
#define EDR_USER_MAX_NITS       430
#define EDR_HARDWARE_MAX_NITS   600
static uint32_t gNitsCap;               /* brightness-nits-cap as last set (16.16) */
static unsigned gNitsSaid;

static void
backlightSay(const char * format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

/*
 * edr=1: the new architecture alone, its maximum the user's (430 nits, 0x7ff): no ExtrabrightEDR, so libEDR never takes
 * the backlight over and nothing the slider asks for goes past 0x7ff. edr=2: and ExtrabrightEDR -- the hardware's 600
 * (a placeholder) over the user's 430, user-accessible-max-nits published -- for HDR, which wants CoreAnimation's EDR
 * dimming working with it (run 210942: with it on and nothing dimmed, the slider's low end came out brighter and the
 * position snapped to 0/1 as libEDR's ramps met CoreBrightness's own). The table: plain nits at evenly spaced slider
 * positions (CoreBrightness interpolates between neighbours; 20F66), roughly logarithmic for even steps to the eye,
 * from lMinPanel to lMaxPanel. Currents: mA x 65536, for its power reports only.
 */
static void
edrPublish(IOService * service)
{
	static const uint32_t kMarketing430[] = { 2, 3, 5, 7, 11, 16, 23, 33, 47, 65, 90, 120, 160, 210, 270, 345, EDR_USER_MAX_NITS };
	static const uint32_t kMarketing600[] = { 2, 3, 5, 8, 12, 19, 30, 46, 70, 100, 140, 190, 250, 320, 400, 490,
		                                  EDR_HARDWARE_MAX_NITS };
	bool extra = gEdr >= 2;
	unsigned long long top = NITS(extra ? EDR_HARDWARE_MAX_NITS : EDR_USER_MAX_NITS);
	const struct { const char * key; unsigned long long value; } kCalibration[] = {
		{ "current-for-max-backlight", NITS(20) }, { "current-for-mid-backlight", 350617 /* 5.35 mA */ },
		{ "hardware-max-current-limit", NITS(28) },
		{ "lMaxProduct", top }, { "lMidProduct", NITS(140) }, { "lMinProduct", NITS(2) },
		{ "lMaxPanel", top }, { "lMidPanel", NITS(140) }, { "lMinPanel", NITS(2) },
	};
	const uint32_t * kMarketing = extra ? kMarketing600 : kMarketing430;
	const unsigned entries = 17;
	OSDictionary * calibration = OSDictionary::withCapacity(sizeof(kCalibration) / sizeof(kCalibration[0]));
	uint8_t table[17 * 4], user[4];
	uint32_t userMax = (uint32_t) NITS(EDR_USER_MAX_NITS);

	if (calibration == NULL) {
		return;
	}
	for (unsigned i = 0; i < sizeof(kCalibration) / sizeof(kCalibration[0]); i++) {
		OSNumber * n = OSNumber::withNumber(kCalibration[i].value, 32);

		calibration->setObject(kCalibration[i].key, n);
		OSSafeReleaseNULL(n);
	}
	for (unsigned i = 0; i < entries; i++) {
		uint32_t v = (uint32_t) NITS(kMarketing[i]);

		for (unsigned b = 0; b < 4; b++) {
			table[4 * i + b] = (uint8_t)(v >> (8 * b));
		}
	}
	for (unsigned b = 0; b < 4; b++) {
		user[b] = (uint8_t)(userMax >> (8 * b));
	}
	OSData * tableData = OSData::withBytes(table, sizeof(table));
	OSData * userData = OSData::withBytes(user, sizeof(user));

	if (calibration->getCount() == sizeof(kCalibration) / sizeof(kCalibration[0]) && tableData != NULL && userData != NULL) {
		service->setProperty("backlight-calibration-parameters", calibration);
		service->setProperty("backlight-marketing-table", tableData);
		if (extra) {
			service->setProperty("user-accessible-max-nits", userData);
		}
		service->setProperty("new-backlight-architecture", kOSBooleanTrue);
		backlightSay("MiniAppleARMBacklight: the new architecture (edr=%u): calibration published, %u entries up to %u nits%s\n",
		    gEdr, entries, (unsigned)(top >> 16), extra ? ", the user's 430: ExtrabrightEDR" : ", no ExtrabrightEDR");
	} else {
		backlightSay("MiniAppleARMBacklight: EDR (edr=1): the calibration could not be made: the old architecture stays\n");
	}
	OSSafeReleaseNULL(calibration);
	OSSafeReleaseNULL(tableData);
	OSSafeReleaseNULL(userData);
}

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties,
	    IOUserClient ** handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

bool
AppleARMBacklight::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setName("backlight");
	/* what the power logger (UserEventAgent's PerfPowerServices plugin) matches on, with IOPropertyMatch, and reads back */
	setProperty("backlight-control", kOSBooleanTrue);
	{
		OSDictionary * brightness = OSDictionary::withCapacity(3);
		OSDictionary * parameters = OSDictionary::withCapacity(1);

		if (brightness != NULL && parameters != NULL) {
			brightness->setObject("min", OSNumber::withNumber((unsigned long long) 0, 32));
			brightness->setObject("max", OSNumber::withNumber((unsigned long long) 65536, 32));
			brightness->setObject("value", OSNumber::withNumber((unsigned long long) 32768, 32));
			parameters->setObject("brightness", brightness);
			setProperty("IODisplayParameters", parameters);
		}
		OSSafeReleaseNULL(brightness);
		OSSafeReleaseNULL(parameters);
	}
	if (gEdr) {
		edrPublish(this);
	}
	registerService();
	return true;
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	char procname[32] = "?";
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);

	proc_selfname(procname, sizeof(procname));
	if (dict == NULL) {
		IOLog("MiniAppleARMBacklight: %s[%d] set properties of a kind that is not a dictionary\n", procname, proc_selfpid());
		return kIOReturnBadArgument;
	}
	OSCollectionIterator * keys = OSCollectionIterator::withCollection(dict);
	const OSSymbol * key;

	while (keys != NULL && (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL) {
		OSObject * value = dict->getObject(key);
		OSNumber * number = OSDynamicCast(OSNumber, value);

		OSSerialize * text = OSSerialize::withCapacity(256);

		if (text != NULL && value != NULL && value->serialize(text)) {
			IOLog("MiniAppleARMBacklight: %s[%d] sets %s = %.300s\n", procname, proc_selfpid(), key->getCStringNoCopy(),
			    text->text());
			if (gEdr && gNitsSaid++ < 80) {
				/* (the new architecture's way is being learnt: every property, where the loader's log keeps it) */
				backlightSay("MiniAppleARMBacklight: %s[%d] sets %s = %.200s\n", procname, proc_selfpid(),
				    key->getCStringNoCopy(), text->text());
			}
		} else {
			IOLog("MiniAppleARMBacklight: %s[%d] sets %s = %s %llu\n", procname, proc_selfpid(), key->getCStringNoCopy(),
			    value ? value->getMetaClass()->getClassName() : "?", number ? number->unsigned64BitValue() : 0);
		}
		OSSafeReleaseNULL(text);
		if (value != NULL && gEdr && key->isEqualTo("DisplayBrightness")) {
			/*
			 * Not kept (edr=1): its nits are CFNumbers of floating point, which the kernel's OSNumber holds as their bits
			 * (an integer of size 31); read back, CoreBrightness says "Unexpected nits type" and takes the brightness back
			 * to where it was -- every step of the slider undone (run 205218). Nor is it the brightness: it reports the
			 * slider's position (CoreBrightness's __DisplayUpdateSlider, 20F66); brightness-nits is what the panel is to
			 * show, every step of a fade (SetBLDriverNitsValue). Applying both made two drivers of the panel fight -- the
			 * owner's Control Center slider swept the range twice in its first 30% (run 210107). "Unexpected nits type"
			 * itself is CoreBrightness's own message about its own notification: on an iPhone X too, nothing to do here.
			 */
		} else if (value != NULL) {
			setProperty(key, value);
			/* "backlight-level" and "brightness" arrive together: 0..65536 (16.16: 0x8000 is half); one is enough */
			if (number != NULL && key->isEqualTo("backlight-level")) {
				hk_display_set_backlight((uint32_t) number->unsigned64BitValue());
			}
			/* the new architecture (edr=1): nits x 65536, and the cap EDR may go to */
			if (gEdr && number != NULL && key->isEqualTo("brightness-nits-cap")) {
				gNitsCap = (uint32_t) number->unsigned64BitValue();
			}
			if (gEdr && number != NULL && key->isEqualTo("brightness-nits")) {
				uint32_t nits16 = number->unsigned32BitValue();

				hk_display_set_nits(gNitsCap != 0 && nits16 > gNitsCap ? gNitsCap : nits16, gNitsCap);
			}
		}
	}
	OSSafeReleaseNULL(keys);
	return kIOReturnSuccess;
}

IOReturn
AppleARMBacklight::newUserClient(task_t, void *, UInt32 type, OSDictionary *, IOUserClient ** handler)
{
	char procname[32] = "?";

	proc_selfname(procname, sizeof(procname));
	IOLog("MiniAppleARMBacklight: %s[%d] opens a user client of type %u\n", procname, proc_selfpid(), (unsigned) type);
	if (gEdr) {
		backlightSay("MiniAppleARMBacklight: %s[%d] opens a user client of type %u: none here\n", procname, proc_selfpid(),
		    (unsigned) type);
	}
	*handler = NULL;
	return kIOReturnUnsupported;
}

void
MiniAppleARMBacklightStart(void)
{
	IOService * platform = (IOService *) IOService::getPlatform();
	AppleARMBacklight * light = OSTypeAlloc(AppleARMBacklight);

	if (platform == NULL || light == NULL || !light->init() || !light->attach(platform)) {
		OSSafeReleaseNULL(light);
		IOLog("MiniAppleARMBacklight: cannot create the service\n");
		return;
	}
	if (!light->start(platform)) {
		light->detach(platform);
	}
	light->release();
}
