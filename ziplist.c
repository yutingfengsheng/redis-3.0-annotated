/* Memory layout of a ziplist, containing "foo", "bar", "quux":
 * <zlbytes><zllen><len>"foo"<len>"bar"<len>"quux"
 *
 * <zlbytes> is an unsigned integer to hold the number of bytes that
 * the ziplist occupies. This is stored to not have to traverse the ziplist
 * to know the new length when pushing.
 *
 * <zllen> is the number of items in the ziplist. When this value is
 * greater than 254, we need to traverse the entire list to know
 * how many items it holds.
 *
 * <len> is the number of bytes occupied by a single entry. When this
 * number is greater than 253, the length will occupy 5 bytes, where
 * the extra bytes contain an unsigned integer to hold the length.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "zmalloc.h"
#include "sds.h"
#include "ziplist.h"

/* Important note: the ZIP_END value is used to depict the end of the
 * ziplist structure. When a pointer contains an entry, the first couple
 * of bytes contain the encoded length of the previous entry. This length
 * is encoded as ZIP_ENC_RAW length, so the first two bits will contain 00
 * and the byte will therefore never have a value of 255. */
#define ZIP_END 255
#define ZIP_BIGLEN 254

/* Entry encoding */
#define ZIP_ENC_RAW     0
#define ZIP_ENC_SHORT   1
#define ZIP_ENC_INT     2
#define ZIP_ENC_LLONG   3
#define ZIP_ENCODING(p) ((p)[0] >> 6)

/* Length encoding for raw entries */
#define ZIP_LEN_INLINE  0
#define ZIP_LEN_UINT16  1
#define ZIP_LEN_UINT32  2

/* Utility macros */
#define ZIPLIST_BYTES(zl) (*((unsigned int*)(zl)))
#define ZIPLIST_TAIL_OFFSET(zl) (*((zl)+sizeof(unsigned int)))
#define ZIPLIST_LENGTH(zl) (*((zl)+2*sizeof(unsigned int)))
#define ZIPLIST_HEADER_SIZE (2*sizeof(unsigned int)+1)
#define ZIPLIST_INCR_LENGTH(zl,incr) { \
    if (ZIPLIST_LENGTH(zl) < ZIP_BIGLEN) ZIPLIST_LENGTH(zl)+=incr; }

typedef struct zlentry {
    unsigned int prevrawlensize, prevrawlen;
    unsigned int lensize, len;
    unsigned int headersize;
    unsigned char encoding;
} zlentry;

/* Return bytes needed to store integer encoded by 'encoding' */
static unsigned int zipEncodingSize(char encoding) {
    if (encoding == ZIP_ENC_SHORT) {
        return sizeof(short int);
    } else if (encoding == ZIP_ENC_INT) {
        return sizeof(int);
    } else if (encoding == ZIP_ENC_LLONG) {
        return sizeof(long long);
    }
    assert(NULL);
}

/* Decode the encoded length pointed by 'p'. If a pointer to 'lensize' is
 * provided, it is set to the number of bytes required to encode the length. */
static unsigned int zipDecodeLength(unsigned char *p, unsigned int *lensize) {
    unsigned char encoding = ZIP_ENCODING(p), lenenc;
    unsigned int len;

    if (encoding == ZIP_ENC_RAW) {
        lenenc = (p[0] >> 4) & 0x3;
        if (lenenc == ZIP_LEN_INLINE) {
            len = p[0] & 0xf;
            if (lensize) *lensize = 1;
        } else if (lenenc == ZIP_LEN_UINT16) {
            len = p[1] | (p[2] << 8);
            if (lensize) *lensize = 3;
        } else {
            len = p[1] | (p[2] << 8) | (p[3] << 16) | (p[4] << 24);
            if (lensize) *lensize = 5;
        }
    } else {
        len = zipEncodingSize(encoding);
        if (lensize) *lensize = 1;
    }
    return len;
}

/* Encode the length 'l' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
static unsigned int zipEncodeLength(unsigned char *p, char encoding, unsigned int rawlen) {
    unsigned char len = 1, lenenc, buf[5];
    if (encoding == ZIP_ENC_RAW) {
        if (rawlen <= 0xf) {
            if (!p) return len;
            lenenc = ZIP_LEN_INLINE;
            buf[0] = rawlen;
        } else if (rawlen <= 0xffff) {
            len += 2;
            if (!p) return len;
            lenenc = ZIP_LEN_UINT16;
            buf[1] = (rawlen     ) & 0xff;
            buf[2] = (rawlen >> 8) & 0xff;
        } else {
            len += 4;
            if (!p) return len;
            lenenc = ZIP_LEN_UINT32;
            buf[1] = (rawlen      ) & 0xff;
            buf[2] = (rawlen >>  8) & 0xff;
            buf[3] = (rawlen >> 16) & 0xff;
            buf[4] = (rawlen >> 24) & 0xff;
        }
        buf[0] = (lenenc << 4) | (buf[0] & 0xf);
    }
    if (!p) return len;

    /* Apparently we need to store the length in 'p' */
    buf[0] = (encoding << 6) | (buf[0] & 0x3f);
    memcpy(p,buf,len);
    return len;
}

/* Return the difference in number of bytes needed to store the new length
 * "len" on the entry pointed to by "p". */
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize;
    zipDecodeLength(p,&prevlensize);
    return zipEncodeLength(NULL,ZIP_ENC_RAW,len)-prevlensize;
}

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'.
 * Warning: this function requires a NULL-terminated string! */
static int zipTryEncoding(unsigned char *entry, long long *v, char *encoding) {
    long long value;
    char *eptr;

    if (entry[0] == '-' || (entry[0] >= '0' && entry[0] <= '9')) {
        value = strtoll((char*)entry,&eptr,10);
        if (eptr[0] != '\0') return 0;
        if (value >= SHRT_MIN && value <= SHRT_MAX) {
            *encoding = ZIP_ENC_SHORT;
        } else if (value >= INT_MIN && value <= INT_MAX) {
            *encoding = ZIP_ENC_INT;
        } else {
            *encoding = ZIP_ENC_LLONG;
        }
        *v = value;
        return 1;
    }
    return 0;
}

/* Store integer 'value' at 'p', encoded as 'encoding' */
static void zipSaveInteger(unsigned char *p, long long value, char encoding) {
    short int s;
    int i;
    long long l;
    if (encoding == ZIP_ENC_SHORT) {
        s = value;
        memcpy(p,&s,sizeof(s));
    } else if (encoding == ZIP_ENC_INT) {
        i = value;
        memcpy(p,&i,sizeof(i));
    } else if (encoding == ZIP_ENC_LLONG) {
        l = value;
        memcpy(p,&l,sizeof(l));
    } else {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p' */
static long long zipLoadInteger(unsigned char *p, char encoding) {
    short int s;
    int i;
    long long l, ret;
    if (encoding == ZIP_ENC_SHORT) {
        memcpy(&s,p,sizeof(s));
        ret = s;
    } else if (encoding == ZIP_ENC_INT) {
        memcpy(&i,p,sizeof(i));
        ret = i;
    } else if (encoding == ZIP_ENC_LLONG) {
        memcpy(&l,p,sizeof(l));
        ret = l;
    } else {
        assert(NULL);
    }
    return ret;
}

/* Return a struct with all information about an entry. */
static zlentry zipEntry(unsigned char *p) {
    zlentry e;
    e.prevrawlen = zipDecodeLength(p,&e.prevrawlensize);
    e.len = zipDecodeLength(p+e.prevrawlensize,&e.lensize);
    e.headersize = e.prevrawlensize+e.lensize;
    e.encoding = ZIP_ENCODING(p+e.prevrawlensize);
    return e;
}

/* Return the total number of bytes used by the entry at "p". */
static unsigned int zipRawEntryLength(unsigned char *p) {
    zlentry e = zipEntry(p);
    return e.headersize + e.len;
}

/* Create a new empty ziplist. */
unsigned char *ziplistNew(void) {
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;
    unsigned char *zl = zmalloc(bytes);
    ZIPLIST_BYTES(zl) = bytes;
    ZIPLIST_TAIL_OFFSET(zl) = ZIPLIST_HEADER_SIZE;
    ZIPLIST_LENGTH(zl) = 0;
    zl[bytes-1] = ZIP_END;
    return zl;
}

/* Resize the ziplist. */
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
    zl = zrealloc(zl,len);
    ZIPLIST_BYTES(zl) = len;
    zl[len-1] = ZIP_END;
    return zl;
}

static unsigned char *ziplistHead(unsigned char *zl) {
    return zl+ZIPLIST_HEADER_SIZE;
}

static unsigned char *ziplistTail(unsigned char *zl) {
    unsigned char *p, *q;
    p = q = ziplistHead(zl);
    while (*p != ZIP_END) {
        q = p;
        p += zipRawEntryLength(p);
    }
    return q;
}

unsigned char *ziplistPush(unsigned char *zl, unsigned char *entry, unsigned int elen, int where) {
    unsigned int curlen = ZIPLIST_BYTES(zl), reqlen, prevlen;
    unsigned char *p, *curtail;
    char encoding = ZIP_ENC_RAW;
    long long value;

    /* We need to store the length of the current tail when the list
     * is non-empty and we push at the tail. */
    curtail = zl+ZIPLIST_TAIL_OFFSET(zl);
    if (where == ZIPLIST_TAIL && curtail[0] != ZIP_END) {
        prevlen = zipRawEntryLength(curtail);
    } else {
        prevlen = 0;
    }

    /* See if the entry can be encoded */
    if (zipTryEncoding(entry,&value,&encoding)) {
        reqlen = zipEncodingSize(encoding);
    } else {
        reqlen = elen;
    }

    /* We need space for both the length of the previous entry and
     * the length of the payload. */
    reqlen += zipEncodeLength(NULL,ZIP_ENC_RAW,prevlen);
    reqlen += zipEncodeLength(NULL,encoding,elen);

    /* Resize the ziplist and move if needed */
    zl = ziplistResize(zl,curlen+reqlen);
    if (where == ZIPLIST_HEAD) {
        p = zl+ZIPLIST_HEADER_SIZE;
        if (*p != ZIP_END) {
            /* Subtract one because of the ZIP_END bytes */
            memmove(p+reqlen,p,curlen-ZIPLIST_HEADER_SIZE-1);
        }
    } else {
        p = zl+curlen-1;
    }

    /* Update tail offset if this is not the first element */
    if (curtail[0] != ZIP_END) {
        if (where == ZIPLIST_HEAD) {
            ZIPLIST_TAIL_OFFSET(zl) += reqlen;
        } else {
            ZIPLIST_TAIL_OFFSET(zl) += prevlen;
        }
    }

    /* Write the entry */
    p += zipEncodeLength(p,ZIP_ENC_RAW,prevlen);
    p += zipEncodeLength(p,encoding,elen);
    if (encoding != ZIP_ENC_RAW) {
        zipSaveInteger(p,value,encoding);
    } else {
        memcpy(p,entry,elen);
    }
    ZIPLIST_INCR_LENGTH(zl,1);
    return zl;
}

unsigned char *ziplistPop(unsigned char *zl, sds *target, int where) {
    unsigned int curlen = ZIPLIST_BYTES(zl), rawlen;
    zlentry entry;
    int nextdiff = 0;
    unsigned char *p;
    long long value;
    if (target) *target = NULL;

    /* Get pointer to element to remove */
    p = (where == ZIPLIST_HEAD) ? ziplistHead(zl) : ziplistTail(zl);
    if (*p == ZIP_END) return zl;

    entry = zipEntry(p);
    rawlen = entry.headersize+entry.len;
    if (target) {
        if (entry.encoding == ZIP_ENC_RAW) {
            *target = sdsnewlen(p+entry.headersize,entry.len);
        } else {
            value = zipLoadInteger(p+entry.headersize,entry.encoding);
            *target = sdscatprintf(sdsempty(), "%lld", value);
        }
    }

    if (where == ZIPLIST_HEAD) {
        /* The next entry will now be the head of the list */
        if (p[rawlen] != ZIP_END) {
            /* Tricky: storing the length of the previous entry in the next
             * entry (this previous length is always 0 when popping from the
             * head), might reduce the number of bytes needed.
             *
             * In this special case (new length is 0), we know that the
             * byte difference to store is always <= 0, which means that
             * we always have space to store it. */
            nextdiff = zipPrevLenByteDiff(p+rawlen,0);
            zipEncodeLength(p+rawlen-nextdiff,ZIP_ENC_RAW,0);
        }
        /* Move list to the front */
        memmove(p,p+rawlen-nextdiff,curlen-ZIPLIST_HEADER_SIZE-rawlen+nextdiff);

        /* Subtract the gained space from the tail offset */
        ZIPLIST_TAIL_OFFSET(zl) -= rawlen+nextdiff;
    } else {
        /* Subtract the length of the previous element from the tail offset. */
        ZIPLIST_TAIL_OFFSET(zl) -= entry.prevrawlen;
    }

    /* Resize and update length */
    zl = ziplistResize(zl,curlen-rawlen+nextdiff);
    ZIPLIST_INCR_LENGTH(zl,-1);
    return zl;
}

/* Returns an offset to use for iterating with ziplistNext. */
unsigned char *ziplistIndex(unsigned char *zl, unsigned int index) {
    unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
    unsigned int i = 0;
    for (; i < index; i++) {
        if (*p == ZIP_END) break;
        p += zipRawEntryLength(p);
    }
    return p;
}

/* Return pointer to next entry in ziplist. */
unsigned char *ziplistNext(unsigned char *p) {
    return *p == ZIP_END ? p : p+zipRawEntryLength(p);
}

/* Get entry pointer to by 'p' and store in either 'e' or 'v' depending
 * on the encoding of the entry. 'e' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the zipmap, 1 otherwise. */
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    if (*p == ZIP_END) return 0;
    if (sstr) *sstr = NULL;

    entry = zipEntry(p);
    if (entry.encoding == ZIP_ENC_RAW) {
        if (sstr) {
            *slen = entry.len;
            *sstr = p+entry.headersize;
        }
    } else {
        if (sval) {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);
        }
    }
    return 1;
}

/* Delete a range of entries from the ziplist. */
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {
    unsigned char *p, *first = ziplistIndex(zl, index);
    unsigned int i, totlen, deleted = 0;
    for (p = first, i = 0; *p != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p);
        deleted++;
    }

    totlen = p-first;
    if (totlen > 0) {
        /* Move current tail to the new tail when there *is* a tail */
        if (*p != ZIP_END) memmove(first,p,ZIPLIST_BYTES(zl)-(p-zl)-1);

        /* Resize and update length */
        zl = ziplistResize(zl, ZIPLIST_BYTES(zl)-totlen);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
    }
    return zl;
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. */
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    unsigned int offset = *p-zl, tail, len;
    len = zipRawEntryLength(*p);
    tail = ZIPLIST_BYTES(zl)-offset-len-1;

    /* Move current tail to the new tail when there *is* a tail */
    if (tail > 0) memmove(*p,*p+len,tail);

    /* Resize and update length */
    zl = ziplistResize(zl, ZIPLIST_BYTES(zl)-len);
    ZIPLIST_INCR_LENGTH(zl,-1);

    /* Store new pointer to current element in p.
     * This needs to be done because zl can change on realloc. */
    *p = zl+offset;
    return zl;
}

/* Compare entry pointer to by 'p' with 'entry'. Return 1 if equal. */
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long val, sval;
    if (*p == ZIP_END) return 0;

    entry = zipEntry(p);
    if (entry.encoding == ZIP_ENC_RAW) {
        /* Raw compare */
        if (entry.len == slen) {
            return memcmp(p+entry.headersize,sstr,slen) == 0;
        } else {
            return 0;
        }
    } else {
        /* Try to compare encoded values */
        if (zipTryEncoding(sstr,&sval,&sencoding)) {
            if (entry.encoding == sencoding) {
                val = zipLoadInteger(p+entry.headersize,entry.encoding);
                return val == sval;
            }
        }
    }
    return 0;
}

/* Return length of ziplist. */
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    if (ZIPLIST_LENGTH(zl) < ZIP_BIGLEN) {
        len = ZIPLIST_LENGTH(zl);
    } else {
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;
        while (*p != ZIP_END) {
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough */
        if (len < ZIP_BIGLEN) ZIPLIST_LENGTH(zl) = len;
    }
    return len;
}

/* Return size in bytes of ziplist. */
unsigned int ziplistSize(unsigned char *zl) {
    return ZIPLIST_BYTES(zl);
}

void ziplistRepr(unsigned char *zl) {
    unsigned char *p;
    zlentry entry;

    printf("{total bytes %d} {length %u}\n",ZIPLIST_BYTES(zl), ZIPLIST_LENGTH(zl));
    p = ziplistHead(zl);
    while(*p != ZIP_END) {
        entry = zipEntry(p);
        printf("{offset %ld, header %u, payload %u} ",p-zl,entry.headersize,entry.len);
        p += entry.headersize;
        if (entry.encoding == ZIP_ENC_RAW) {
            fwrite(p,entry.len,1,stdout);
        } else {
            printf("%lld", zipLoadInteger(p,entry.encoding));
        }
        printf("\n");
        p += entry.len;
    }
    printf("{end}\n\n");
}

#ifdef ZIPLIST_TEST_MAIN

unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

int main(int argc, char **argv) {
    unsigned char *zl, *p, *q, *entry;
    unsigned int elen;
    long long value;
    sds s;

    zl = createIntList();
    ziplistRepr(zl);

    zl = createList();
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_TAIL);
    printf("Pop tail: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_HEAD);
    printf("Pop head: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_TAIL);
    printf("Pop tail: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    zl = ziplistPop(zl, &s, ZIPLIST_TAIL);
    printf("Pop tail: %s (length %ld)\n", s, sdslen(s));
    ziplistRepr(zl);

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                fwrite(entry,elen,1,stdout);
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
        }
        printf("\n");
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            if (entry && strncmp("foo", entry, elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl, &p);
            } else {
                printf("Entry: ");
                if (entry) {
                    fwrite(entry,elen,1,stdout);
                } else {
                    printf("%lld", value);
                }
                p = ziplistNext(p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        if (!ziplistCompare(p,"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return;
        }
        if (ziplistCompare(p,"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return;
        }

        p = ziplistIndex(zl, 3);
        if (!ziplistCompare(p,"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return;
        }
        if (ziplistCompare(p,"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return;
        }
        printf("SUCCESS\n");
    }

    return 0;
}
#endif
