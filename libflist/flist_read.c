#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <blake2.h>
#include <libgen.h>
#include "libflist.h"
#include "verbose.h"
#include "flist_serial.h"
#include "flist_write.h"
#include "flist_dirnode.h"

#define KEYLENGTH 16

//
// directory object reader
//

#if 0
directory_t *flist_directory_get(flist_db_t *database, char *key, char *fullpath) {
    directory_t *dir;

    if(!(dir = malloc(sizeof(directory_t))))
        diep("directory: malloc");

    // reading capnp message from database
    dir->value = database->sget(database, key);

    // FIXME: memory leak
    if(!dir->value->data) {
        debug("[-] libflist: directory: key [%s, %s] not found\n", key, fullpath);
        return NULL;
    }

    // build capn context
    if(capn_init_mem(&dir->ctx, (unsigned char *) dir->value->data, dir->value->length, 0)) {
        debug("[-] libflist: directory: capnp: init error\n");
        database->clean(dir->value);
        // FIXME: memory leak
        return NULL;
    }

    // populate dir struct from context
    // the contents is always a directory (one key per directory)
    // and the whole contents is on the content field
    dir->dirp.p = capn_getp(capn_root(&dir->ctx), 0, 1);
    read_Dir(&dir->dir, dir->dirp);

    return dir;
}

void flist_directory_close(flist_db_t *database, directory_t *dir) {
    database->clean(dir->value);
    capn_free(&dir->ctx);
    free(dir);
}

#endif

//
// helpers
//
char *libflist_path_key(char *path) {
    uint8_t hash[KEYLENGTH];

    if(blake2b(hash, path, "", KEYLENGTH, strlen(path), 0) < 0) {
        debug("[-] libflist: blake2 error\n");
        return NULL;
    }

    return libflist_hashhex(hash, KEYLENGTH);
}

char *flist_clean_path(char *path) {
    size_t offset, length;

    offset = 0;
    length = strlen(path);

    if(length == 0)
        return calloc(1, 1);

    // remove lead slash
    if(path[0] == '/')
        offset = 1;

    // remove trailing slash
    if(path[length - 1] == '/')
        length -= 1;

    return strndup(path + offset, length);
}

//
// public helpers
//
#if 0
flist_acl_t *libflist_mk_permissions(char *uname, char *gname, int mode) {
    flist_acl_t *acl;

    if(!(acl = malloc(sizeof(flist_acl_t))))
        return NULL;

    acl->uname = strdup(uname);
    acl->gname = strdup(gname);
    acl->mode = mode;

    return acl;
}
#endif

#if 0
int flist_fileindex_from_name(directory_t *direntry, char *filename) {
    Inode_ptr inodep;
    struct Inode inode;

    for(int i = 0; i < capn_len(direntry->dir.contents); i++) {
        inodep.p = capn_getp(direntry->dir.contents.p, i, 1);
        read_Inode(&inode, inodep);

        if(strcmp(inode.name.str, filename) == 0)
            return i;
    }

    return -1;
}
#endif

inode_t *libflist_inode_from_name(dirnode_t *root, char *filename) {
    for(inode_t *inode = root->inode_list; inode; inode = inode->next) {
        if(strcmp(inode->name, filename) == 0)
            return inode;
    }

    return NULL;
}

#if 0
// populate an acl object from a racl pointer
flist_acl_t *libflist_racl_to_acl(acl_t *dst, flist_acl_t *src) {
    dst->uname = src->uname;
    dst->gname = src->gname;
    dst->mode = src->mode;
    dst->key = libflist_inode_acl_key(dst);

    return src;
}
#endif

inode_t *flist_inode_from_dirnode(dirnode_t *dirnode) {
    inode_t *inode;

    if(!(inode = inode_create(dirnode->name, 4096, dirnode->fullpath)))
        return NULL;

    inode->type = INODE_DIRECTORY;
    inode->modification = dirnode->modification;
    inode->creation = dirnode->creation;
    inode->subdirkey = strdup(dirnode->hashkey);
    inode->acl = dirnode->acl; // FIXME: DUPLICATE ACL

    return inode;
}

/*
dirnode_t *flist_dirnode_duplicate(dirnode_t *source) {
    dirnode_t *dirnode;

    if(!(dirnode = calloc(sizeof(dirnode_t), 1)))
        return NULL;

    // setting directory metadata
    dirnode->fullpath = strdup(inode->fullpath);
    dirnode->name = strdup(inode->name);
    dirnode->hashkey = libflist_path_key(inode->fullpath);
    dirnode->creation = inode->creation;
    dirnode->modification = inode->modification;
    dirnode->acl = inode->acl;

    return dirnode;
}
*/

inode_t *libflist_directory_create(dirnode_t *parent, char *name) {
    inode_t *inode = libflist_inode_mkdir(name, parent);
    flist_dirnode_appends_inode(parent, inode);

    dirnode_t *dirnode = flist_dirnode_from_inode(inode);
    flist_dirnode_appends_dirnode(parent, dirnode);

    return inode;
}


