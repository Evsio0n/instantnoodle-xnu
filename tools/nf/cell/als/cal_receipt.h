#ifndef CELL_ALS_CAL_RECEIPT_H
#define CELL_ALS_CAL_RECEIPT_H
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* Fixed receipt belongs to this handset's immutable QA root. The source
 * bytes still come from its own persist inode, never the receipt. */
#define ALS_FACTORY_RECEIPT "/hackios/fw/als-calibration-249.sha256"

static bool
als_match_receipt(const uint8_t *file, unsigned size, const uint8_t *expected)
{
    uint8_t hash[CC_SHA256_DIGEST_LENGTH];
    if (size != 4 || !CC_SHA256(file, size, hash)) return false;
    return memcmp(hash, expected, sizeof(hash)) == 0;
}

static bool
als_current_capture(struct ce_fs *fs, const uint8_t *file, unsigned size)
{
    uint8_t expected[CC_SHA256_DIGEST_LENGTH], verification[CE_BS];
    if (!(fs->inc & 4)) return true;
    if (fs->inc != 0x46 || fs->ro != 0x7b || size != 4) return false;
    int fd = open(ALS_FACTORY_RECEIPT, O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd < 0) return false;
    struct stat sb;
    bool valid = fstat(fd,&sb) == 0 && S_ISREG(sb.st_mode) && sb.st_uid == 0 &&
        !(sb.st_mode & 022) && sb.st_size == (off_t)sizeof(expected) &&
        pread(fd,expected,sizeof(expected),0) == (ssize_t)sizeof(expected);
    close(fd);
    if (!valid || !als_match_receipt(file,size,expected)) return false;
    /* Re-read only the original pathname, inode, extent and content blocks
     * from the read-only device. Reject inconsistent snapshots at any level. */
    for (unsigned i=0;i<fs->count;i++) {
        const struct ce_slot *slot=&fs->cache[i];
        if (!fs->read(fs->source,slot->number,verification) ||
            memcmp(slot->data,verification,sizeof(verification))) return false;
    }
    return true;
}
#endif
