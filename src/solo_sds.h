#ifndef SOLO_SDS_H
#define SOLO_SDS_H

#include <stddef.h>

typedef char *sds;

size_t sdslen(const sds s);
sds sdsnewlen(const void *init, size_t len);
sds sdsnew(const char *init);
sds sdsempty(void);
sds sdsdup(const sds s);
void sdsfree(sds s);
sds sdscatlen(sds s, const void *t, size_t len);
int sdscmp(const sds a, const sds b);
void sdssetlen(sds s, size_t len);

#endif
