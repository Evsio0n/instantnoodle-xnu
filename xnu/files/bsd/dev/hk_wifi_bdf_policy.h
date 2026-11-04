/* Explicit policy for the captured IN2010 and public ath11k 05266 pair.
 * Native CNSS: absent board TLV -> 0xff; non-GF chip + ELF -> bdwlan.elf;
 * converged DT adds qca6390/. The image stores that exact modem file flat.
 * This does not claim the original Android selected it at runtime, nor that
 * all RF calibration has been validated. Other reported board IDs fail closed. */
#ifndef HK_WIFI_BDF_POLICY_H
#define HK_WIFI_BDF_POLICY_H
#define HK_WIFI_NATIVE_BDF_FILE "qca6390-bdwlan.elf"
static bool
hk_wifi_native_bdf_eligible(const struct hk_wifi_cap *cap, bool seen)
{
 return seen && (cap->fields & 9u) == 9u && cap->chip == 0 && cap->family == 0xbu &&
  cap->firmware == 0x10121492u && (!(cap->fields & 2u) || cap->board == 0xffu);
}
#endif
