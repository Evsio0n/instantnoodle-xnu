/* Real mission NAS channels. A public registry entry advertises the existence
 * of a bounded kernel transport, with its actual local and remote endpoints.
 * This port lacks a trust-enforcing native ABP client path. Every normal user
 * open is denied, even for UID 0. No Apple control or interface class is named. */
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSString.h>
#include <sys/hk_qmux_frontend.h>

class HKCellularQMUXChannel : public IOService
{
    OSDeclareDefaultStructors(HKCellularQMUXChannel);

  public:
    bool matchPropertyTable(OSDictionary *table) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(HKCellularQMUXChannel, IOService);

bool HKCellularQMUXChannel::matchPropertyTable(OSDictionary *table)
{
    if (!table || !IOService::matchPropertyTable(table))
        return false;
    OSObject *property = table->getObject("ABPInterfaceProtocol");
    if (!property)
        return true;
    OSString *requested = OSDynamicCast(OSString, property);
    OSString *actual = OSDynamicCast(OSString, getProperty("ABPInterfaceProtocol"));
    return requested && actual && actual->isEqualTo(requested);
}

IOReturn HKCellularQMUXChannel::newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **out)
{
    if (out)
        *out = nullptr;
    return kIOReturnNotPermitted;
}

static HKCellularQMUXChannel *nodes[2];
static uint64_t published_generation[2];

extern "C" bool hk_qmux_registry_withdraw(unsigned index, uint64_t generation)
{
    if (index < 1 || index > 2)
        return false;
    unsigned i = index - 1;
    HKCellularQMUXChannel *node = nodes[i];
    if (!node)
        return true;
    if (generation != published_generation[i])
        return false;
    /* An IOKit observer can retain the inactive object; it owns immutable
     * scalar properties and no handle to kernel buffers or the modem. */
    if (!node->isInactive() && !node->terminate(kIOServiceRequired))
        return false;
    node->release();
    nodes[i] = nullptr;
    published_generation[i] = 0;
    return true;
}

extern "C" bool hk_qmux_registry_publish(const struct hk_qmux_channel_info *channel)
{
    if (!channel || channel->index < 1 || channel->index > 2 || !channel->generation || !channel->node ||
        !channel->remote_port || !channel->local_port || !channel->cid)
        return false;
    unsigned i = channel->index - 1;
    IOService *parent = IOService::getResourceService();
    if (nodes[i] || !parent)
        return false;
    HKCellularQMUXChannel *node = new HKCellularQMUXChannel;
    if (!node)
        return false;
    if (!node->init()) {
        node->release();
        return false;
    }
    node->setName(i ? "HKCellularQMUX2" : "HKCellularQMUX1");
    if (!node->setProperty("ABPInterfaceProtocol", i ? "qmux2" : "qmux1") ||
        !node->setProperty("HKTransportGeneration", channel->generation, 64) ||
        !node->setProperty("HKQRTRNode", channel->node, 32) ||
        !node->setProperty("HKQRTRPeerPort", channel->remote_port, 32) ||
        !node->setProperty("HKQRTRLocalPort", channel->local_port, 32) ||
        !node->setProperty("HKQMUXClient", channel->cid, 8) || !node->attach(parent)) {
        node->release();
        return false;
    }
    if (!node->start(parent)) {
        node->detach(parent);
        node->release();
        return false;
    }
    nodes[i] = node;
    published_generation[i] = channel->generation;
    node->registerService();
    return true;
}
