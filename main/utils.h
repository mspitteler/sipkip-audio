#ifndef UTILS_H
#define UTILS_H

#include <inttypes.h>
#include <string.h>
#include <stdio.h>

#include "sdkconfig.h"

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

static inline char *bd_address_to_string(uint8_t *bda, char *str, size_t size) {
    if (bda == NULL || str == NULL || size < 18) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}

static inline char *dirname(char *path) {
    static const char dot[] = ".";
    char *last_slash;

    /* Find last '/'.  */
    last_slash = path != NULL ? strrchr(path, '/') : NULL;

    if (last_slash == path)
        /* The last slash is the first character in the string.  We have to
           return "/".  */
        ++last_slash;
    else if (last_slash != NULL && last_slash[1] == '\0')
        /* The '/' is the last character, we have to look further.  */
        last_slash = memchr(path, last_slash - path, '/');

    if (last_slash != NULL)
        /* Terminate the path.  */
        last_slash[0] = '\0';
    else
        /* This assignment is ill-designed but the XPG specs require to
           return a string containing "." in any case no directory part is
           found and so a static and constant string is required.  */
        path = (char *)dot;

    return path;
}

static inline char *readable_file_size(size_t size /* in bytes */, char *buf) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
    int i = 0;
    size *= 1000;
    while (size >= 1024000) {
        size >>= 10;
        i++;
    }
    sprintf(buf, "%lu.%lu %s", size / 1000, size % 1000, units[i]);
    return buf;
}

static inline int copy_file(const char *src, const char *dst) {
    FILE *in = NULL, *out = NULL;
    size_t in_size, out_size;
    char buf[CONFIG_LITTLEFS_PAGE_SIZE];
    int ret_val = 0;
    
    in = fopen(src, "rb");
    if (!in)
        return -1;
    
    out = fopen(dst, "wb");
    if (!out)
        ret_val = -1;
    
    while (!ret_val) {
        in_size = fread(buf, sizeof(*buf), sizeof(buf) / sizeof(*buf), in);
        if (in_size == 0)
            break;
        
        out_size = fwrite(buf, sizeof(*buf), in_size, out);
        if (out_size == 0)
            ret_val = -1;
    }
    
    fclose(in);
    if (out)
        fclose(out);
    return ret_val;
}

#endif /* UTILS_H */
