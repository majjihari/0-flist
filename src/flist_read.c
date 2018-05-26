#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <rocksdb/c.h>
#include <blake2.h>
#include "flister.h"
#include "flist_read.h"
#include "flist.capnp.h"

// reading a flist -- iterating -- walking
//
// flist is a rocksdb database
// - each directory is one key-value object
// - each acl are stored and dedupe on the db
//
// - one directory is identified by blake2 hash of it's full path
// - the root directory is the blake2 hash of empty string
//

typedef struct directory_t {
    value_t *value;
    struct capn ctx;
    Dir_ptr dirp;
    struct Dir dir;

} directory_t;

typedef struct walker_t {
    database_t *database;
    void (*callback)(struct walker_t *, directory_t *, void *userptr);

} walker_t;

#define KEYLENGTH 32

static int flist_directory_walk(walker_t *walker, const char *key, void *userptr);

//
// directory object reader
//
static directory_t *flist_directory_get(database_t *database, const char *key) {
    directory_t *dir;

    if(!(dir = malloc(sizeof(directory_t))))
        diep("directory: malloc");

    // reading capnp message from database
    dir->value = database_get(database, key);

    if(!dir->value->data) {
        fprintf(stderr, "[-] directory: key [%s] not found\n", key);
        return NULL;
    }

    // build capn context
    if(capn_init_mem(&dir->ctx, (unsigned char *) dir->value->data, dir->value->length, 0)) {
        fprintf(stderr, "[-] directory: capnp: init error\n");
        database_value_free(dir->value);
        return NULL;
    }

    // populate dir struct from context
    // the contents is always a directory (one key per directory)
    // and the whole contents is on the content field
    dir->dirp.p = capn_getp(capn_root(&dir->ctx), 0, 1);
    read_Dir(&dir->dir, dir->dirp);

    return dir;
}

static void flist_directory_close(directory_t *dir) {
    database_value_free(dir->value);
    capn_free(&dir->ctx);
    free(dir);
}

//
// helpers
//
static const char *pathkey(char *path) {
    uint8_t hash[KEYLENGTH];
    char *hexhash;

    if(blake2b(hash, path, "", KEYLENGTH, strlen(path), 0) < 0) {
        fprintf(stderr, "[-] blake2 error\n");
        exit(EXIT_FAILURE);
    }

    if(!(hexhash = malloc(sizeof(char) * ((KEYLENGTH * 2) + 1))))
        diep("malloc");

    for(int i = 0; i < KEYLENGTH; i++)
        sprintf(hexhash + (i * 2), "%02x", hash[i]);

    return (const char *) hexhash;
}

//
// FIXME: ugly, use bitwise
//
static char *permsingle(char value) {
    if(value == '0') return "---";
    if(value == '1') return "--x";
    if(value == '2') return "-w-";
    if(value == '3') return "-wx";
    if(value == '4') return "r--";
    if(value == '5') return "r-x";
    if(value == '6') return "rw-";
    if(value == '7') return "rwx";
    return "???";
}

//
// FIXME: ugly, use bitwise
//
static char *permstr(unsigned int mode, char *modestr, size_t slen) {
    char octstr[16];

    if(slen < 12)
        return NULL;


    int length = snprintf(octstr, sizeof(octstr), "%o", mode);
    if(length < 3 && length > 6) {
        strcpy(modestr, "?????????");
        return NULL;
    }

    strcpy(modestr, permsingle(octstr[length - 3]));
    strcpy(modestr + 3, permsingle(octstr[length - 2]));
    strcpy(modestr + 6, permsingle(octstr[length - 1]));

    return modestr;
}

// generate a tree dump of all entries
static void flist_tree(walker_t *walker, directory_t *root, void *userptr) {
    int level = 0;

    if(userptr)
        level = *((int *) userptr);

    for(int i = 0; i < level; i++)
        printf("    ");

    printf("| /%s\n", root->dir.location.str);

    Inode_ptr inodep;
    struct Inode inode;

    for(int i = 0; i < capn_len(root->dir.contents); i++) {
        inodep.p = capn_getp(root->dir.contents.p, i, 1);
        read_Inode(&inode, inodep);

        for(int i = 0; i < level; i++)
            printf("    ");

        struct SubDir sub;
        struct Link link;

        // writing inode information
        switch(inode.attributes_which) {
            case Inode_attributes_dir:
                read_SubDir(&sub, inode.attributes.dir);

                printf("+-- %s\n", inode.name.str);

                int newlevel = level + 1;
                flist_directory_walk(walker, sub.key.str, &newlevel);
                break;

            case Inode_attributes_link:
                read_Link(&link, inode.attributes.link);

                printf("| %s -> %s\n", inode.name.str, link.target.str);
                break;

            case Inode_attributes_special:
            case Inode_attributes_file:
                printf("| %s\n", inode.name.str);
                break;
        }
    }
}

// dumps contents using kind of 'ls -al' view
// generate a list with type, permissions, size, blocks, name
static void flist_ls(walker_t *walker, directory_t *root, void *userptr) {
    (void) userptr;

    printf("/%s:\n", root->dir.location.str);
    // printf("[+] directory: /%s\n", dir.location.str);
    // printf("Parent: %s\n", dir.parent.str);
    // printf("Size: %lu\n", dir.size);

    Inode_ptr inodep;
    struct Inode inode;

    // iterating over the whole directory
    // contents
    for(int i = 0; i < capn_len(root->dir.contents); i++) {
        // reading the next entry
        inodep.p = capn_getp(root->dir.contents.p, i, 1);
        read_Inode(&inode, inodep);

        value_t *perms;

        // checking if this entry is another directory
        // (a subdirectory)
        if(inode.attributes_which == Inode_attributes_dir) {
            struct SubDir sub;
            read_SubDir(&sub, inode.attributes.dir);

            // if this is a subdirectory
            // we need to load that directory in order to
            // find it's acl (directory acl are defined on
            // the object itself)
            directory_t *subdir;
            if(!(subdir = flist_directory_get(walker->database, sub.key.str)))
                return;

            // reading directory permissions
            perms = database_get(walker->database, subdir->dir.aclkey.str);
            if(!perms->data)
                dies("directory entry: cannot get acl from database");

            flist_directory_close(subdir);

        } else {
            // reading file permissions
            perms = database_get(walker->database, inode.aclkey.str);
            if(!perms->data)
                dies("inode entry: cannot get acl from database");
        }

        ACI_ptr acip;
        struct ACI aci;

        struct capn permsctx;
        if(capn_init_mem(&permsctx, (unsigned char *) perms->data, perms->length, 0)) {
            fprintf(stderr, "[-] capnp: init error\n");
        }

        acip.p = capn_getp(capn_root(&permsctx), 0, 1);
        read_ACI(&aci, acip);

        char modestr[16];
        permstr(aci.mode, modestr, sizeof(modestr));

        const char *uname = (aci.uname.len) ? aci.uname.str : "????";
        const char *gname = (aci.gname.len) ? aci.gname.str : "????";

        // writing inode information
        switch(inode.attributes_which) {
            case Inode_attributes_dir:
                printf("d%s  %-8s %-8s ", modestr, uname, gname);

                struct SubDir sub;
                read_SubDir(&sub, inode.attributes.dir);

                printf("%8lu (       --- ) %-12s\n", inode.size, inode.name.str);
                break;

            case Inode_attributes_file: ;
                struct File file;
                read_File(&file, inode.attributes.file);

                printf("-%s  %-8s %-8s ", modestr, uname, gname);
                printf("%8lu (%4d blocks) %s\n", inode.size, capn_len(file.blocks), inode.name.str);
                break;

            case Inode_attributes_link:
                printf("lrwxrwxrwx  %-8s %-8s ", uname, gname);

                struct Link link;
                read_Link(&link, inode.attributes.link);

                printf("%8lu (       --- ) %s -> %s\n", inode.size, inode.name.str, link.target.str);
                break;

            case Inode_attributes_special:
                printf("b%s  %-8s %-8s ", modestr, uname, gname);
                printf("%8lu %s\n", inode.size, inode.name.str);
                break;
        }
    }

    // newline between directories
    printf("\n");

    // now, each entries (files) are displayed
    // walking over sub-directories
    for(int i = 0; i < capn_len(root->dir.contents); i++) {
        // reading again this entry
        inodep.p = capn_getp(root->dir.contents.p, i, 1);
        read_Inode(&inode, inodep);

        // if this is a directory, walking inside
        if(inode.attributes_which == Inode_attributes_dir) {
            struct SubDir sub;
            read_SubDir(&sub, inode.attributes.dir);

            // we don't need userptr
            flist_directory_walk(walker, sub.key.str, NULL);
        }
    }
}

static void flist_blocks(walker_t *walker, directory_t *root, void *userptr) {
    (void) userptr;
    Inode_ptr inodep;
    struct Inode inode;

    for(int i = 0; i < capn_len(root->dir.contents); i++) {
        inodep.p = capn_getp(root->dir.contents.p, i, 1);
        read_Inode(&inode, inodep);

        // print full path
        if(root->dir.location.len) {
            printf("/%s/%s\n", root->dir.location.str, inode.name.str);

        } else printf("/%s\n", inode.name.str);

        if(inode.attributes_which == Inode_attributes_file) {
            struct File file;
            read_File(&file, inode.attributes.file);

            FileBlock_ptr blockp;
            struct FileBlock block;

            for(int i = 0; i < capn_len(file.blocks); i++) {
                blockp.p = capn_getp(file.blocks.p, i, 1);
                read_FileBlock(&block, blockp);

                printf("  hash: %.*s\n", block.hash.p.len, block.hash.p.data);
                printf("  key : %.*s\n", block.key.p.len, block.key.p.data);
            }
        }

        // walking over internal directories
        if(inode.attributes_which == Inode_attributes_dir) {
            struct SubDir sub;
            read_SubDir(&sub, inode.attributes.dir);

            // recursive list contents
            flist_directory_walk(walker, sub.key.str, NULL);
        }
    }
}

// dumps (in a 'find' way) file contents
// simple print one line per entries, with full path
static void flist_dump(walker_t *walker, directory_t *root, void *userptr) {
    (void) userptr;
    Inode_ptr inodep;
    struct Inode inode;

    for(int i = 0; i < capn_len(root->dir.contents); i++) {
        inodep.p = capn_getp(root->dir.contents.p, i, 1);
        read_Inode(&inode, inodep);

        // print full path
        if(root->dir.location.len) {
            printf("/%s/%s\n", root->dir.location.str, inode.name.str);

        } else printf("/%s\n", inode.name.str);

        // walking over internal directories
        if(inode.attributes_which == Inode_attributes_dir) {
            struct SubDir sub;
            read_SubDir(&sub, inode.attributes.dir);

            // recursive list contents
            flist_directory_walk(walker, sub.key.str, NULL);
        }
    }
}

//
// walker
//
static int flist_directory_walk(walker_t *walker, const char *key, void *userptr) {
    directory_t *dir;

    if(!(dir = flist_directory_get(walker->database, key)))
        return 1;

    // walking over this directory
    walker->callback(walker, dir, userptr);

    // cleaning this directory
    flist_directory_close(dir);

    return 0;
}

// walking entry point
int flist_walk(database_t *database) {
    walker_t walker = {
        .database = database,
        .callback = flist_ls,
    };

    // root directory is an empty key
    const char *key = pathkey("");

    // walking starting from the root
    flist_directory_walk(&walker, key, NULL);

    // cleaning
    free((char *) key);

    return 0;
}