/* A completed scan without a foreign-channel event gave no TX window.
 * Briefly prefer another legal channel the same peer actually advertises. */
static uint64_t roc_retry_after[178];
static bool roc_recently_unavailable(unsigned channel,uint64_t now)
{
    return channel<178 && now<__atomic_load_n(&roc_retry_after[channel],__ATOMIC_RELAXED);
}
static void roc_record_outcome(unsigned channel,bool granted,bool timed_out,uint64_t now)
{
    if(channel>=178 || (!granted && !timed_out))return;
    __atomic_store_n(&roc_retry_after[channel],granted?0:now+16000000ull,__ATOMIC_RELAXED);
}
