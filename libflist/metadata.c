#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "libflist.h"
#include "verbose.h"

int libflist_metadata_set(flist_db_t *database, char *metadata, char *payload) {
    if(database->mdset(database, metadata, payload)) {
        debug("[-] libflist: metadata: set: %s\n", libflist_strerror());
        return 0;
    }

    debug("[-] libflist: metadata: <%s> set into database\n", metadata);
    return 1;
}

char *libflist_metadata_get(flist_db_t *database, char *metadata) {
    value_t *rawdata = database->mdget(database, metadata);

    if(!rawdata->data) {
        debug("[-] libflist: metadata: get: metadata not found\n");
        free(rawdata);
        return NULL;
    }

    printf("[+] libflist: metadata: value for <%s>\n", metadata);
    printf("[+] %s\n", rawdata->data);

    char *value = rawdata->data;
    free(rawdata);

    return value;
}

int libflist_metadata_remove(flist_db_t *database, char *metadata) {
    if(database->mddel(database, metadata)) {
        debug("[-] libflist: metadata: del: %s\n", libflist_strerror());
        return 0;
    }

    debug("[-] libflist: metadata: <%s> deleted\n", metadata);
    return 1;
}
