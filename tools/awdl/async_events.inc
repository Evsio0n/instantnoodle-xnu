/* iOS 20F66 AppleDevice asynchronous fallback. Selector 0 supplies a
 * process buffer of 2048-byte slots. Selector 1 returns a consumed slot. */
#include <IOKit/IOMemoryDescriptor.h>

class HKAWDLEventClient : public IOUserClient {
    OSDeclareDefaultStructors(HKAWDLEventClient);
public:
    task_t owner;
    IOMemoryDescriptor *buffer;
    OSAsyncReference64 callback;
    uint32_t slots;
    uint32_t occupied;
    bool registered;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn registerNotificationPort(mach_port_t, UInt32, io_user_reference_t) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *,
                            OSObject *, void *) APPLE_KEXT_OVERRIDE;
    void disconnect();
    void notifyLink(bool up);
};
OSDefineMetaClassAndStructors(HKAWDLEventClient, IOUserClient);
static HKAWDLEventClient *subscriber;
static bool currentLinkUp;

void HKAWDLEventClient::disconnect()
{
    lck_mtx_lock(&awdlRegistryLock);
    if (subscriber==this) subscriber=NULL;
    IOMemoryDescriptor *previous=buffer;
    buffer=NULL;
    if (registered) releaseAsyncReference64(callback);
    registered=false;
    lck_mtx_unlock(&awdlRegistryLock);
    if(previous){previous->complete();previous->release();}
}
IOReturn HKAWDLEventClient::clientClose()
{
    disconnect();
    if(!isInactive()) terminate();
    return kIOReturnSuccess;
}
void HKAWDLEventClient::stop(IOService *provider)
{
    disconnect();
    IOUserClient::stop(provider);
}
IOReturn HKAWDLEventClient::registerNotificationPort(mach_port_t port,UInt32 type,io_user_reference_t)
{
    /* The asynchronous invocation takes its own wake-port reference. */
    if(port) releaseNotificationPort(port);
    return type==0?kIOReturnSuccess:kIOReturnUnsupported;
}
/* Serialized with registration, slot acknowledgements and disconnect. */
void HKAWDLEventClient::notifyLink(bool up)
{
    unsigned slot=0;
    while(slot<slots && (occupied&(1u<<slot)))++slot;
    if(slot==slots)return;
    uint8_t link[32]={};
    link[0]=!up;
    if(buffer->writeBytes(2048ull*slot,link,sizeof(link))!=sizeof(link))return;
    io_user_reference_t args[]={4,sizeof(link),slot}; // APPLE80211_M_LINK_CHANGED
    occupied|=1u<<slot;
    IOReturn ret=sendAsyncResult64WithOptions(callback,kIOReturnSuccess,args,3,kIOUserNotifyOptionCanDrop);
    if(ret)occupied&=~(1u<<slot);
    IOLog("awdl-event: link up=%u slot=%u sent=%x\n",up,slot,ret);
}
extern "C" void hk_awdl_notify_link(bool up)
{
    lck_mtx_lock(&awdlRegistryLock);
    currentLinkUp=up;
    if(subscriber)subscriber->notifyLink(up);
    lck_mtx_unlock(&awdlRegistryLock);
}
IOReturn HKAWDLEventClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,
    IOExternalMethodDispatch *,OSObject *,void *)
{
    if(!a || !a->structureInput || !a->asyncReference || !a->asyncWakePort)return kIOReturnBadArgument;
    if(selector==1 && a->structureInputSize==4){
        uint32_t slot;
        memcpy(&slot,a->structureInput,sizeof(slot));
        lck_mtx_lock(&awdlRegistryLock);
        bool valid=registered && slot<slots && (occupied&(1u<<slot));
        if(valid)occupied&=~(1u<<slot);
        lck_mtx_unlock(&awdlRegistryLock);
        if(!valid)return kIOReturnBadArgument;
        releaseAsyncReference64(a->asyncReference);
        IOLog("awdl-event: consumed slot=%u\n",slot);
        return kIOReturnSuccess;
    }
    if(selector!=0 || a->structureInputSize!=16 || a->asyncReferenceCount!=8)return kIOReturnUnsupported;
    struct {uint64_t address;uint32_t count,pad;} request;
    memcpy(&request,a->structureInput,sizeof(request));
    IOLog("awdl-event: register slots=%u\n",request.count);
    if(!request.address || !request.count || request.count>1024)return kIOReturnBadArgument;
    uint32_t used=MIN(request.count,32u);
    auto *mem=IOMemoryDescriptor::withAddressRange(request.address,2048ull*used,kIODirectionIn,owner);
    if(!mem)return kIOReturnNoMemory;
    IOReturn ret=mem->prepare();
    if(ret){mem->release();return ret;}
    lck_mtx_lock(&awdlRegistryLock);
    if(isInactive() || subscriber || registered){
        lck_mtx_unlock(&awdlRegistryLock);mem->complete();mem->release();return kIOReturnBusy;
    }
    slots=used;occupied=0;buffer=mem;registered=true;subscriber=this;
    memcpy(callback,a->asyncReference,sizeof(callback));
    notifyLink(currentLinkUp);
    lck_mtx_unlock(&awdlRegistryLock);
    return kIOReturnSuccess;
}

class HKAWDLInterface : public IONetworkInterface {
    OSDeclareDefaultStructors(HKAWDLInterface);
public:
    IOReturn newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,
                           IOUserClient **out) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(HKAWDLInterface, IONetworkInterface);
IOReturn HKAWDLInterface::newUserClient(task_t task,void *security,UInt32 type,
                                       OSDictionary *properties,IOUserClient **out)
{
    IOLog("awdl-event: open type=%u\n",type);
    if(!out || (type!=3 && type!=4))return kIOReturnUnsupported;
    *out=NULL;
    HKAWDLEventClient *client=OSTypeAlloc(HKAWDLEventClient);
    if(!client)return kIOReturnNoMemory;
    client->owner=task;
    if(!client->initWithTask(task,security,type,properties) || !client->attach(this)){
        client->release();return kIOReturnError;
    }
    if(!client->start(this)){
        client->detach(this);client->release();return kIOReturnError;
    }
    *out=client;
    return kIOReturnSuccess;
}
