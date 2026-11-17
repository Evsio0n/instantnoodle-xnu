/* Included below HKWLAN in a private build. All names, addressing and MTU
 * originate in the live BSD interfaces. The provider must hold its firmware
 * generation and serialize final BSD detachment until remove returns zero. */
#include <kern/locks.h>
#include <net/if_private.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <sys/errno.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>

static LCK_GRP_DECLARE(awdlRegistryGroup, "hk-awdl-registry");
static LCK_MTX_DECLARE(awdlRegistryLock, &awdlRegistryGroup);

#include "async_events.inc"

static struct {
    IONetworkInterface *node;   /* owned references, no data-plane callbacks */
    ifnet_t interface;
    uint64_t lease;
    uint64_t retired;
    bool busy;
} awdlRegistry;

static bool
hkStringEquals(IORegistryEntry *entry, const char *key, const char *value)
{
    OSObject *property = entry->copyProperty(key);
    OSString *string = OSDynamicCast(OSString, property);
    bool matches = string && string->isEqualTo(value);
    OSSafeReleaseNULL(property);
    return matches;
}

/* Return a retained controller of the live, unique Infrastructure station.
 * The original station path obtains its address from the BSD driver when
 * HKWLANStart runs. Recheck it against today's attached en0. */
static int
hkCopyAwDLController(IOService **result)
{
    ifnet_t station = NULL;
    int error = ifnet_find_by_name("en0", &station);
    if (error)
        return error;
    uint8_t address[6];
    error = ifnet_lladdr_copy_bytes(station, address, sizeof(address));
    ifnet_release(station);
    if (error)
        return error;

    OSDictionary *matching = IOService::serviceMatching("HKWLAN");
    if (!matching)
        return ENOMEM;
    OSIterator *matches = IOService::getMatchingServices(matching);
    matching->release();
    if (!matches)
        return ENXIO;
    IOService *controller = NULL;
    OSObject *found;
    while ((found = matches->getNextObject()) != NULL) {
        HKWLAN *infra = OSDynamicCast(HKWLAN, found);
        if (!infra || infra->isInactive() ||
            !hkStringEquals(infra, "IOInterfaceName", "en0") ||
            !hkStringEquals(infra, "IO80211InterfaceRole", "Infrastructure"))
            continue;
        IORegistryEntry *parent = infra->copyParentEntry(gIOServicePlane);
        IO80211Controller *candidate = OSDynamicCast(IO80211Controller, parent);
        OSObject *property = candidate ? candidate->copyProperty("IOMACAddress") : NULL;
        OSData *mac = OSDynamicCast(OSData, property);
        bool valid = candidate && !candidate->isInactive() && mac &&
            mac->getLength() == sizeof(address) &&
            !memcmp(mac->getBytesNoCopy(), address, sizeof(address));
        OSSafeReleaseNULL(property);
        if (valid && controller) {
            OSSafeReleaseNULL(parent);
            error = EEXIST;
            break;
        }
        if (valid) {
            controller = candidate;          /* adopts the copied parent */
        } else {
            OSSafeReleaseNULL(parent);
        }
    }
    matches->release();
    if (error || !controller) {
        OSSafeReleaseNULL(controller);
        return error ? error : ENXIO;
    }
    *result = controller;
    return 0;
}

static int
hkVerifyAwDLInterface(const char *name, const uint8_t mac[6], uint32_t mtu, ifnet_t *result)
{
    ifnet_t interface = NULL;
    int error = ifnet_find_by_name(name, &interface);
    if (error)
        return error;
    char canonical[IFNAMSIZ];
    snprintf(canonical, sizeof(canonical), "awdl%u", ifnet_unit(interface));
    uint8_t actual[6];
    if (strcmp(name, canonical) || strcmp(ifnet_name(interface), "awdl") ||
        !(ifnet_eflags(interface) & IFEF_AWDL) || ifnet_type(interface) != IFT_ETHER) {
        error = ENXIO;
    } else if (ifnet_mtu(interface) != mtu) {
        error = ESTALE;
    } else {
        error = ifnet_lladdr_copy_bytes(interface, actual, sizeof(actual));
        if (!error && memcmp(actual, mac, sizeof(actual)))
            error = ESTALE;
    }
    if (error) {
        ifnet_release(interface);
        return error;
    }
    *result = interface;
    return 0;
}

static int
hkCreateAwDLNode(IOService *parent, ifnet_t interface, const char *name,
    const uint8_t mac[6], uint32_t mtu, IONetworkInterface **result)
{
    IONetworkInterface *node = OSTypeAlloc(HKAWDLInterface);
    if (!node)
        return ENOMEM;
    if (!node->init() ||
        !node->setProperty("IOInterfaceName", name) ||
        !node->setProperty("BSD Name", name) ||
        !node->setProperty("IOInterfaceNamePrefix", "awdl") ||
        !node->setProperty("IOInterfaceUnit", ifnet_unit(interface), 32) ||
        !node->setProperty("IOInterfaceType", ifnet_type(interface), 32) ||
        !node->setProperty("IOMaxTransferUnit", mtu, 32) ||
        !node->setProperty("IOMACAddress", const_cast<uint8_t *>(mac), 6) ||
        !node->setProperty("IOPrimaryInterface", false) ||
        !node->setProperty("IO80211VirtualInterfaceRole", "AirLink")) {
        node->release();
        return ENOMEM;
    }
    if (parent->isInactive() || !node->attach(parent)) {
        node->release();
        return ENXIO;
    }
    node->setName(name);
    if (!node->start(parent)) {
        node->detach(parent);
        node->release();
        return ENXIO;
    }
    node->registerService(kIOServiceSynchronous);
    *result = node;
    return 0;
}

extern "C" int
hk_awdl_registry_publish(uint64_t lease, const char *name, const uint8_t mac[6], uint32_t mtu)
{
    if (!lease || !name || strnlen(name, IFNAMSIZ) == IFNAMSIZ || !mac || !mtu)
        return EINVAL;
    lck_mtx_lock(&awdlRegistryLock);
    int error = 0;
    if (awdlRegistry.busy)
        error = EBUSY;
    else if (lease <= awdlRegistry.retired || (awdlRegistry.node && awdlRegistry.lease != lease))
        error = ESTALE;
    if (error) {
        lck_mtx_unlock(&awdlRegistryLock);
        return error;
    }
    awdlRegistry.busy = true;
    IONetworkInterface *existing = awdlRegistry.node;
    ifnet_t original = awdlRegistry.interface;
    lck_mtx_unlock(&awdlRegistryLock);

    ifnet_t interface = NULL;
    IOService *parent = NULL;
    IONetworkInterface *node = NULL;
    error = hkVerifyAwDLInterface(name, mac, mtu, &interface);
    if (!error)
        error = hkCopyAwDLController(&parent);
    if (!error) {
        if (existing) {
            if (original != interface || existing->isInactive() || existing->getProvider() != parent)
                error = ESTALE;
        } else {
            error = hkCreateAwDLNode(parent, interface, name, mac, mtu, &node);
        }
    }
    OSSafeReleaseNULL(parent);

    lck_mtx_lock(&awdlRegistryLock);
    if (node) {
        awdlRegistry.node = node;
        awdlRegistry.interface = interface;
        awdlRegistry.lease = lease;
        interface = NULL;
    }
    awdlRegistry.busy = false;
    lck_mtx_unlock(&awdlRegistryLock);
    if (interface)
        ifnet_release(interface);
    return error;
}

extern "C" int
hk_awdl_registry_remove(uint64_t lease)
{
    if (!lease)
        return EINVAL;
    lck_mtx_lock(&awdlRegistryLock);
    if (awdlRegistry.busy) {
        lck_mtx_unlock(&awdlRegistryLock);
        return EBUSY;
    }
    if (lease < awdlRegistry.retired || (awdlRegistry.node && lease != awdlRegistry.lease)) {
        lck_mtx_unlock(&awdlRegistryLock);
        return ESTALE;
    }
    IONetworkInterface *node = awdlRegistry.node;
    if (!node) {
        /* Also retires a partial create which never published a node. */
        awdlRegistry.retired = lease;
        lck_mtx_unlock(&awdlRegistryLock);
        return 0;
    }
    awdlRegistry.busy = true;
    ifnet_t interface = awdlRegistry.interface;
    lck_mtx_unlock(&awdlRegistryLock);

    if (!node->isInactive())
        node->terminate(kIOServiceSynchronous);
    bool detached = node->isInactive() && !node->inPlane(gIOServicePlane);
    lck_mtx_lock(&awdlRegistryLock);
    if (detached) {
        awdlRegistry.node = NULL;
        awdlRegistry.interface = NULL;
        awdlRegistry.lease = 0;
        awdlRegistry.retired = lease;
    }
    awdlRegistry.busy = false;
    lck_mtx_unlock(&awdlRegistryLock);
    if (!detached)
        return EBUSY;
    ifnet_release(interface);
    node->release();
    return 0;
}
