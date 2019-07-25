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

#define KEYLENGTH 16

#define discard __attribute__((cleanup(__cleanup_free)))

static void __cleanup_free(void *p) {
    free(* (void **) p);
}

//
// directory object reader
//
static inode_chunks_t *capnp_inode_to_chunks(struct Inode *inode) {
    inode_chunks_t *blocks;

    struct File file;
    read_File(&file, inode->attributes.file);

    // allocate empty blocks
    if(!(blocks = calloc(sizeof(inode_chunks_t), 1)))
        return NULL;

    blocks->size = capn_len(file.blocks);
    blocks->blocksize = 0;

    if(!(blocks->list = (inode_chunk_t *) malloc(sizeof(inode_chunk_t) * blocks->size)))
        return NULL;

    for(size_t i = 0; i < blocks->size; i++) {
        FileBlock_ptr blockp;
        struct FileBlock block;

        blockp.p = capn_getp(file.blocks.p, i, 1);
        read_FileBlock(&block, blockp);

        blocks->list[i].entryid = libflist_bufdup(block.hash.p.data, block.hash.p.len);
        blocks->list[i].entrylen = block.hash.p.len;

        blocks->list[i].decipher = libflist_bufdup(block.key.p.data, block.key.p.len);
        blocks->list[i].decipherlen = block.key.p.len;
    }

    return blocks;
}

static inode_t *flist_itementry_to_inode(flist_db_t *database, struct Dir *dir, int fileindex) {
    inode_t *target;
    Inode_ptr inodep;
    struct Inode inode;

    // pointing to the right item
    // on the contents list
    inodep.p = capn_getp(dir->contents.p, fileindex, 1);
    read_Inode(&inode, inodep);

    // allocate a new inode empty object
    if(!(target = calloc(sizeof(inode_t), 1)))
        return NULL;

    // fill in default information
    target->name = strdup(inode.name.str);
    target->size = inode.size;
    target->fullpath = flist_inode_fullpath(dir, &inode);
    target->creation = inode.creationTime;
    target->modification = inode.modificationTime;

    if(!(target->acl = libflist_get_acl(database, inode.aclkey.str))) {
        inode_free(target);
        return NULL;
    }

    // fill in specific information dependent of
    // the type of the entry
    switch(inode.attributes_which) {
        case Inode_attributes_dir: ;
            struct SubDir sub;
            read_SubDir(&sub, inode.attributes.dir);

            target->type = INODE_DIRECTORY;
            target->subdirkey = strdup(sub.key.str);
            break;

        case Inode_attributes_file: ;
            target->type = INODE_FILE;
            target->chunks = capnp_inode_to_chunks(&inode);
            break;

        case Inode_attributes_link: ;
            struct Link link;
            read_Link(&link, inode.attributes.link);

            target->type = INODE_LINK;
            target->link = strdup(link.target.str);
            break;

        case Inode_attributes_special: ;
            struct Special special;
            read_Special(&special, inode.attributes.special);

            target->type = INODE_SPECIAL;
            target->stype = special.type;

            capn_data capdata = capn_get_data(special.data.p, 0);
            target->sdata = strndup(capdata.p.data, capdata.p.len);
            break;
    }

    return target;
}

static dirnode_t *flist_dir_to_dirnode(flist_db_t *database, struct Dir *dir) {
    dirnode_t *dirnode;

    if(!(dirnode = calloc(sizeof(dirnode_t), 1)))
        return NULL;

    // setting directory metadata
    dirnode->fullpath = strdup(dir->location.str);
    dirnode->name = strdup(dir->name.str);
    dirnode->hashkey = libflist_path_key(dirnode->fullpath);
    dirnode->creation = dir->creationTime;
    dirnode->modification = dir->modificationTime;

    dirnode->acl = libflist_get_acl(database, dir->aclkey.str);

    // iterating over the full contents
    // and add each inode to the inode list of this directory
    for(int i = 0; i < capn_len(dir->contents); i++) {
        inode_t *inode;

        if((inode = flist_itementry_to_inode(database, dir, i)))
            dirnode_appends_inode(dirnode, inode);
    }

    return dirnode;
}

static dirnode_t *flist_dirnode_get(flist_db_t *database, char *key, char *fullpath) {
    value_t *value;
    struct capn capctx;
    Dir_ptr dirp;
    struct Dir dir;

    // reading capnp message from database
    value = database->sget(database, key);

    // FIXME: memory leak
    if(!value->data) {
        debug("[-] libflist: dirnode: key [%s - %s] not found\n", key, fullpath);
        database->clean(value);
        return NULL;
    }

    // build capn context
    if(capn_init_mem(&capctx, (unsigned char *) value->data, value->length, 0)) {
        debug("[-] libflist: dirnode: capnp: init error\n");
        database->clean(value);
        // FIXME: memory leak
        return NULL;
    }

    // populate dir struct from context
    // the contents is always a directory (one key per directory)
    // and the whole contents is on the content field
    dirp.p = capn_getp(capn_root(&capctx), 0, 1);
    read_Dir(&dir, dirp);

    return flist_dir_to_dirnode(database, &dir);
}


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

static char *flist_clean_path(char *path) {
    size_t offset, length;

    offset = 0;
    length = strlen(path);

    // remove lead slash
    if(path[0] == '/')
        offset = 1;

    // remove trailing slash
    if(path[length - 1] == '/')
        length -= 1;

    return strndup(path + offset, length);
}

void inode_chunks_free(inode_t *inode) {
    if(!inode->chunks)
        return;

    for(size_t i = 0; i < inode->chunks->size; i += 1) {
        free(inode->chunks->list[i].entryid);
        free(inode->chunks->list[i].decipher);
    }

    free(inode->chunks->list);
    free(inode->chunks);
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

dirnode_t *flist_directory_from_inode(inode_t *inode) {
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

inode_t *libflist_directory_create(dirnode_t *parent, char *name) {
    inode_t *inode = libflist_inode_mkdir(name, parent);
    dirnode_appends_inode(parent, inode);

    dirnode_t *dirnode = flist_directory_from_inode(inode);
    dirnode_appends_dirnode(parent, dirnode);

    return inode;
}

//
// fetch a directory object from the database
//
dirnode_t *libflist_dirnode_get(flist_db_t *database, char *path) {
    discard char *cleanpath = NULL;

    // we use strict convention to store
    // entry on the database since the id is a hash of
    // the path, the path needs to match exactly
    //
    // all the keys are without leading slash and never have
    // trailing slash, the root directory is an empty string
    //
    //   eg:  /        -> ""
    //        /home    -> "home"
    //        /var/log -> "var/log"
    //
    // to make this function working for mostly everybody, let's
    // clean the path to ensure it should reflect exactly how
    // it's stored on the database
    if(!(cleanpath = flist_clean_path(path)))
        return NULL;

    debug("[+] libflist: dirnode: get: clean path: <%s> -> <%s>\n", path, cleanpath);

    // converting this directory string into a directory
    // hash by the internal way used everywhere, this will
    // give the key required to find entry on the database
    discard char *key = libflist_path_key(cleanpath);
    debug("[+] libflist: dirnode: get: entry key: <%s>\n", key);

    // requesting the directory object from the database
    // the object in the database is packed, this function will
    // return us something decoded and ready to use
    dirnode_t *direntry;
    if(!(direntry = flist_dirnode_get(database, key, cleanpath)))
        return NULL;

    #if 0
    // we now have the full directory contents into memory
    // because the internal object contains everything, but in
    // an internal format, it's time to convert this format
    // into our public interface
    dirnode_t *contents;

    if(!(contents = flist_directory_to_dirnode(database, direntry)))
        return NULL;
    #endif

    // cleaning temporary string allocated
    // by internal functions and not needed objects anymore
    // flist_directory_close(database, direntry);

    return direntry;
}

dirnode_t *libflist_dirnode_get_recursive(flist_db_t *database, char *path) {
    dirnode_t *root = NULL;

    // fetching root directory
    if(!(root = libflist_dirnode_get(database, path)))
        return NULL;

    for(inode_t *inode = root->inode_list; inode; inode = inode->next) {
        // ignoring non-directories
        if(inode->type != INODE_DIRECTORY)
            continue;

        // if it's a directory, loading it's contents
        // and adding it to the directory lists
        dirnode_t *subdir;
        if(!(subdir = libflist_dirnode_get_recursive(database, inode->fullpath)))
            return NULL;

        dirnode_appends_dirnode(root, subdir);
    }

    return root;
}

dirnode_t *libflist_dirnode_get_parent(flist_db_t *database, dirnode_t *root) {
    discard char *copypath = strdup(root->fullpath);
    char *parent = dirname(copypath);

    // no parent
    if(strcmp(parent, root->fullpath) == 0)
        return root;

    return libflist_dirnode_get(database, copypath);
}
