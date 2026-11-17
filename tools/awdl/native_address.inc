/* Hand the first native create address to the radio before add_interface.
 * Creation waits on a different thread from the ATH11K worker. Neither a
 * published ifnet nor a live firmware VDEV changes identity in place. */
static struct {
    bool waiting, supplied;
    uint8_t bytes[6];
} awdl_address;

/* Called from the radio worker. msleep releases the provider lock. */
int lx_os_awdl_initial_address(uint8_t address[6])
{
    struct timespec wait = { .tv_sec = 90 };
    int error = 0;
    HK_AWDL_LOCK();
    if (awdl_address.waiting || awdl.generation) {
        HK_AWDL_UNLOCK();
        return EBUSY;
    }
    awdl_address.waiting = true;
    awdl_address.supplied = false;
    while (!awdl_address.supplied && !error)
        error = msleep(&awdl_address, &awdl_provider_lock, PZERO, "awdl address", &wait);
    if (awdl_address.supplied) {
        memcpy(address, awdl_address.bytes, 6);
        error = 0;
    }
    awdl_address.waiting = false;
    HK_AWDL_UNLOCK();
    return error;
}

/* Called with provider lock held. Explicit callers can arrive together. */
static int awdl_supply_address_locked(const uint8_t address[6])
{
    struct timespec wait = { .tv_sec = 35 };
    if (!awdl_address.waiting && !awdl_address.supplied) return EOPNOTSUPP;
    if (awdl_address.supplied && memcmp(awdl_address.bytes,address,6)) return EADDRNOTAVAIL;
    memcpy(awdl_address.bytes,address,6);
    awdl_address.supplied = true;
    wakeup(&awdl_address);
    while (!awdl.ready) {
        int error = msleep(&awdl.ready, &awdl_provider_lock, PZERO, "awdl ready", &wait);
        if (error) return error;
    }
    return 0;
}
