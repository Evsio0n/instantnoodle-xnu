/*
 * IN2010's physical front sensor. MobileGestalt 20F66 looks for the
 * numeric HID usage pair on a registered IOService. The backlight
 * remains owned by the existing SLPI/panel policy on its normal path.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>

class HKFrontLightSensor : public IOService {
    OSDeclareDefaultStructors(HKFrontLightSensor);
};
OSDefineMetaClassAndStructors(HKFrontLightSensor, IOService);

/* The board initializer publishes before scheduling its SLPI worker.
 * After scheduling, only that worker calls these routines. */
static HKFrontLightSensor *sensor;
static uint64_t lastSampleUs;
static uint64_t lastPublishedUs;
static const uint64_t staleUs = 1500000;  /* physical panel policy timeout */

extern "C" void
hk_front_light_begin(void)
{
    if (sensor)
        return;
    IOService *parent = IOService::getResourceService();
    if (!parent)
        return;
    HKFrontLightSensor *node = new HKFrontLightSensor;
    if (!node)
        return;
    if (!node->init()) {
        node->release();
        return;
    }
    node->setName("Front Ambient Light Sensor");
    if (!node->setProperty("Product", "STK33502") ||
        !node->setProperty("Transport", "Qualcomm SEE") ||
        !node->setProperty("PrimaryUsagePage", 0xff00, 32) ||
        !node->setProperty("PrimaryUsage", 4, 32) || !node->attach(parent)) {
        node->release();
        return;
    }
    if (!node->start(parent)) {
        node->detach(parent);
        node->release();
        return;
    }
    sensor = node;
    node->registerService();
    IOLog("front-light: registered STK33502 usage=ff00/4 waiting for calibrated sample\n");
}

/* One immutable snapshot per publication. luxBits is the genuine nonnegative
 * IEEE-754 lux value from SEE. Nothing is published before a HIGH sample
 * from the verified front calibration path and coefficient. */
struct front_light_sample {
    uint64_t uptimeUs;
    uint64_t sensorTicks;
    uint32_t sequence;
    uint32_t luxBits;
};
extern "C" void
hk_front_light_sample(uint64_t now, uint64_t tick, unsigned seq, unsigned status, uint32_t lux)
{
    if (!now || !tick || !seq || status != 3 || lux >= 0x4f800000u)
        return;
    if (!sensor)
        hk_front_light_begin();
    if (!sensor)
        return;
    lastSampleUs = now;
    if (lastPublishedUs && now >= lastPublishedUs && now - lastPublishedUs < 250000)
        return;
    front_light_sample sample = {now, tick, seq, lux};
    if (sensor->setProperty("CalibratedSample", &sample, sizeof(sample))) {
        if (!lastPublishedUs)
            IOLog("front-light: calibrated sample ready sequence=%u\n", seq);
        lastPublishedUs = now;
    }
}

extern "C" void
hk_front_light_tick(uint64_t now)
{
    if (sensor && lastPublishedUs && (now < lastSampleUs || now - lastSampleUs >= staleUs)) {
        sensor->removeProperty("CalibratedSample");
        lastPublishedUs = 0;
    }
}

extern "C" void
hk_front_light_end(void)
{
    if (!sensor)
        return;
    sensor->removeProperty("CalibratedSample");
    lastSampleUs = 0;
    lastPublishedUs = 0;
    if (!sensor->isInactive() && !sensor->terminate(kIOServiceRequired)) {
        IOLog("front-light: unregister deferred\n");
        return;
    }
    sensor->release();
    sensor = NULL;
    IOLog("front-light: unregistered STK33502\n");
}
