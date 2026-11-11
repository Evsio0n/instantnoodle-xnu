/* apps_std_skel methods 26..28. Include below ssc_output_buf().
 * Directory entries are derived from the mounted config manifest. */
#ifndef HK_SLPI_STD_DIR_H
#define HK_SLPI_STD_DIR_H

#define SSC_CONFIG_DIR "/odm/etc/sensor/config"
#define SSC_VENDOR_CONFIG_DIR "/vendor/etc/sensors/config"
enum { SSC_OPENDIR=0x1a020100, SSC_CLOSEDIR=0x1b010000, SSC_READDIR=0x1c010100 };
enum { SSC_CONFIG_INO=0x60000001, SSC_CONFIG_PARENT_INO=0x60000002 };

struct ssc_dir_cursor {
    uint64_t token;
    unsigned position;
    bool vendor;
};
struct ssc_dirs {
    uint32_t generation;
    struct ssc_dir_cursor cursors[4];
};

static inline bool
ssc_vendor_directory(const char *path)
{
    return !strcmp(path,SSC_VENDOR_CONFIG_DIR) || !strcmp(path,SSC_VENDOR_CONFIG_DIR "/");
}

static inline bool
ssc_config_directory(const char *path)
{
    return !strcmp(path, SSC_CONFIG_DIR) || !strcmp(path, SSC_CONFIG_DIR "/") || ssc_vendor_directory(path);
}

static inline bool
ssc_is_dir_call(uint32_t sc)
{
    return sc==SSC_OPENDIR || sc==SSC_READDIR || sc==SSC_CLOSEDIR;
}

/* File inode numbers agree with the pack's stat operation. */
static inline const char *
ssc_dir_next(struct ssc_dir_cursor *cursor, uint32_t *ino)
{
    if (cursor->position < 2) {
        bool self = cursor->position++ == 0;
        *ino = (self ? SSC_CONFIG_INO : SSC_CONFIG_PARENT_INO) + (cursor->vendor ? 4 : 0);
        return self ? "." : "..";
    }
    const char *base = cursor->vendor ? SSC_VENDOR_CONFIG_DIR "/" : SSC_CONFIG_DIR "/";
    const size_t prefix = strlen(base);
    const unsigned count = sizeof(ssc_entries) / sizeof(ssc_entries[0]);
    while (cursor->position - 2 < count) {
        unsigned index = cursor->position++ - 2;
        const char *path = ssc_entries[index].path;
        if (strncmp(path, base, prefix)) continue;
        const char *name = path + prefix;
        size_t len = strlen(name);
        if (!len || len > 254 || strchr(name, '/')) continue;
        *ino = index + 1;
        return name;
    }
    return NULL;
}

static inline unsigned
ssc_dir_dispatch(struct ssc_dirs *dirs, uint32_t sc, const struct ssc_inv *a,
                 struct ssc_output *out)
{
    if (sc==SSC_OPENDIR) {
        if (a->ni!=2 || a->no!=1 || a->in[0].n<4 || a->out[0]<8 ||
            !ssc_string(a->in[1], ssc_word(a->in[0].p))) return SSC_BADARG;
        if (!ssc_config_directory((const char *)a->in[1].p)) return SSC_NO_FILE;
        for (unsigned i=0; i<sizeof(dirs->cursors)/sizeof(dirs->cursors[0]); i++) {
            struct ssc_dir_cursor *cursor = &dirs->cursors[i];
            if (cursor->token) continue;
            if (dirs->generation==UINT32_MAX) return SSC_FULL;
            uint8_t *prim = ssc_output_buf(out, a->out[0]);
            if (!prim) return SSC_SMALL;
            /* Opaque session token, never a pointer. */
            cursor->token = UINT64_C(0x4449520000000000) | ++dirs->generation;
            cursor->position = 0;
            cursor->vendor = ssc_vendor_directory((const char *)a->in[1].p);
            memcpy(prim, &cursor->token, sizeof(cursor->token));
            return 0;
        }
        return SSC_FULL;
    }
    if (sc!=SSC_READDIR && sc!=SSC_CLOSEDIR) return SSC_UNSUPPORTED;
    if (a->ni!=1 || a->in[0].n<8 ||
        (sc==SSC_READDIR ? a->no!=1 || a->out[0]<264 : a->no!=0))
        return SSC_BADARG;
    uint64_t token;
    memcpy(&token, a->in[0].p, sizeof(token));
    for (unsigned i=0; token && i<sizeof(dirs->cursors)/sizeof(dirs->cursors[0]); i++) {
        struct ssc_dir_cursor *cursor = &dirs->cursors[i];
        if (cursor->token != token) continue;
        if (sc==SSC_CLOSEDIR) {
            memset(cursor, 0, sizeof(*cursor));
            return 0;
        }
        uint8_t *prim = ssc_output_buf(out, a->out[0]);
        if (!prim) return SSC_SMALL;
        uint32_t ino = 0;
        const char *name = ssc_dir_next(cursor, &ino);
        if (name) {
            ssc_put(prim, ino);
            memcpy(prim+4, name, strlen(name)+1);
        }
        /* DIRENT is 260 bytes (ino32, char name[255], pad). */
        ssc_put(prim+260, name==NULL);
        return 0;
    }
    return SSC_BADHANDLE;
}
#endif
