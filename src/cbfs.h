struct cbfs_header {
    uint32_t magic;       /* 0x4F524243 = "CBRC" ... actually see below */
    uint32_t version;
    uint32_t romsize;
    uint32_t bootblocksize;
    uint32_t align;       /* archive alignment (obsolete, always 64) */
    uint32_t offset;      /* offset of first CBFS component from start of ROM */
    uint32_t architecture;
    uint32_t pad[1];
};

struct cbfs_file {
    uint8_t  magic[8];    /* "LARCHIVE" */
    uint32_t len;         /* length of file data, big-endian */
    uint32_t type;        /* file type, big-endian */
    uint32_t attributes_offset; /* big-endian */
    uint32_t offset;      /* offset from start of this header to data, big-endian */
    char     filename[0]; /* null-terminated, padded to align */
};

extern struct cbfs_header *cbfs_find_header(void);
extern void *cbfs_find_file(struct cbfs_header *hdr,const char *name, uint32_t *data_len);