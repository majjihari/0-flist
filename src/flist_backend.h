#ifndef FLIST_BACKEND_H
    #define FLIST_BACKEND_H

    #include "database.h"

    typedef struct inode_chunk_t {
        char *id;
        char *cipher;

    } inode_chunk_t;

    typedef struct chunks_t {
        size_t upsize;  // uploaded size
        size_t length;  // amount of chunks
        inode_chunk_t *chunks;

    } chunks_t;

    typedef struct backend_data_t {
        void *opaque;
        unsigned char *payload;
        size_t length;

    } backend_data_t;

    typedef struct backend_t {
        database_t *database;

    } backend_t;

    // initialize a backend
    backend_t *backend_init_zdb(char *host, int port, char *namespace);

    // write a file into the backend
    chunks_t *upload_inode(backend_t *backend, char *root, char *path, char *filename);

    // clean (and close) a backend object
    void backend_free(backend_t *backend);

    // clean internal chunks
    void chunks_free(chunks_t *chunks);

#if 0

    chunks_t *upload_inode(char *root, char *path, char *filename);
    void upload_inode_flush();

    backend_data_t *download_block(uint8_t *id, size_t idlen, uint8_t *cipher, size_t cipherlen);
    void download_free(backend_data_t *data);

    void chunks_free(chunks_t *chunks);

    // FIXME
    void upload_free();
#endif

#endif
