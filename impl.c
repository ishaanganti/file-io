#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../io300.h"

#ifndef CACHE_SIZE
#define CACHE_SIZE 8
#endif

struct io300_file {
    /* read,write,seek all take a file descriptor as a parameter */
    int fd;
    /* this will serve as our cache */
    char* cache;

    int valid_bytes; 
    int dirty;          
    int head;         
    int cache_start;
    // int file_size;
    /* Used for debugging, keep track of which io300_file is which */
    char* description;
    /* To tell if we are getting the performance we are expecting */
    struct io300_statistics {
        int read_calls;
        int write_calls;
        int seeks;
    } stats;
};

int io300_fetch(struct io300_file* const f);
off_t io300_filesize(struct io300_file* const f); 
/*
    Assert the properties that you would like your file to have at all times.
    Call this function frequently (like at the beginning of each function) to
    catch logical errors early on in development.
*/
static void check_invariants(struct io300_file* f) {
    assert(f != NULL);
    assert(f->cache != NULL);
    assert(f->fd >= 0);

    assert(f->valid_bytes <= CACHE_SIZE && f->valid_bytes >= 0);
    assert(f->cache_start >= 0);
    assert(f->cache_start + f->head >= 0);
    assert(f->dirty == 1 || f->dirty == 0); 
}

struct io300_file* io300_open(const char* const path, char* description) {
    if (path == NULL) {
        fprintf(stderr, "error: null file path\n");
        return NULL;
    }

    int const fd = open(path, O_RDWR | O_CREAT | O_SYNC, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        fprintf(stderr, "error: could not open file: `%s`: %s\n", path,
                strerror(errno));
        return NULL;
    }

    struct io300_file* const ret = malloc(sizeof(*ret));
    if (ret == NULL) {
        fprintf(stderr, "error: could not allocate io300_file\n");
        close(fd);
        return NULL;
    }

    ret->fd = fd;
    ret->cache = malloc(CACHE_SIZE);
    if (ret->cache == NULL) {
        fprintf(stderr, "error: could not allocate file cache\n");
        close(ret->fd);
        free(ret);
        return NULL;
    }
    ret->description = description;

    ret->valid_bytes = 0;
    ret->dirty = 0;
    ret->head = 0;
    ret->cache_start = 0;

    ret->stats.read_calls = 0;
    ret->stats.write_calls = 0;
    ret->stats.seeks = 0;

    dbg(ret, "Just finished initializing file from path: %s\n");
    io300_fetch(ret);



    check_invariants(ret);
    return ret;
}

int io300_seek(struct io300_file* const f, off_t const pos) {
    check_invariants(f);
    if(pos < 0) {
        return (off_t)-1;
    } else {
        f->head = pos - f->cache_start;
    }
    return pos;
}

int io300_close(struct io300_file* const f) {
    check_invariants(f);

#if (DEBUG_STATISTICS == 1)
    printf("stats: {desc: %s, read_calls: %d, write_calls: %d, seeks: %d}\n",
           f->description, f->stats.read_calls, f->stats.write_calls,
           f->stats.seeks);
#endif
    // come back to this, pretty sure this works though. 
    io300_flush(f);  
    close(f->fd);
    free(f->cache);
    free(f);
    return 0;
}

off_t io300_filesize(struct io300_file* const f) {
    check_invariants(f);
    struct stat s;
    int const r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}


int io300_readc(struct io300_file* const f) {
    check_invariants(f);
    f->stats.read_calls++;
    unsigned char c;
    // Outside of full cache
    if (f->head < 0 || f->head >= CACHE_SIZE){
        io300_fetch(f);
        if (f->valid_bytes == 0){
            return -1;
        } else{
            c = f->cache[f->head];
            f->head++;
        }
    }
    // Inside of full cache
    else{
        if (f->valid_bytes <= f->head){
            return -1;
        } else{
            c = f->cache[f->head];
            f->head++;
        }
    }
    return (int) c;
}

int io300_writec(struct io300_file* f, int ch) {
    check_invariants(f);
    f->stats.write_calls++;
    // not in the cache
    if(f->head < 0 || f->head >= CACHE_SIZE) {
        io300_fetch(f);
        f->cache[f->head] = (char) ch;
        f->head++;
        if(f->head > f->valid_bytes) {
            f->valid_bytes = f->head;
        }
        f->dirty = 1;
        return ch;
    }
    // in the cache
    else {
        f->cache[f->head] = (char) ch;
        f->head++;
        if(f->head > f->valid_bytes) {
            f->valid_bytes = f->head;
        }
        f->dirty = 1;
    }

    return ch; 
}

ssize_t io300_read(struct io300_file* const f, char* const buff,
                   size_t const sz) {
    check_invariants(f);
    f->stats.read_calls++;

    if(sz > CACHE_SIZE) { // read larger than cache size
        io300_flush(f);
        lseek(f->fd, f->cache_start + f->head, SEEK_SET);
        ssize_t bytes_read = read(f->fd, buff, sz);
        f->head += (int) bytes_read;
        io300_fetch(f);
        return bytes_read;
    }
    // read outside valid bytes 
    else if(f->head < 0 || f->head + (int)sz >= f->valid_bytes) {
        io300_fetch(f);
        int readable_bytes = (int)sz;
        if(f->valid_bytes < (int)sz) {
            readable_bytes = f->valid_bytes;
        }
        memcpy(buff, f->cache + f->head, readable_bytes);
        f->head += readable_bytes;
        return (ssize_t) readable_bytes;
    }
    // read in cache compeltely
    else {
        memcpy(buff, f->cache + f->head, sz);
        f->head += sz;
        return (ssize_t) sz;
    }
}

ssize_t io300_write(struct io300_file* const f, const char* buff, size_t const sz) {
    check_invariants(f);
    f->stats.write_calls++;
    if(sz > CACHE_SIZE) { // write greater than cache size, let system do it
        io300_flush(f); // think about this deeply
        lseek(f->fd, f->cache_start + f->head, SEEK_SET);
        ssize_t bytes_written = write(f->fd, buff, sz);
        f->head += (int) bytes_written;
        io300_fetch(f);
        return bytes_written;
    }
    else if(f->head >= 0 && (f->head + (int)sz < CACHE_SIZE)) { // write fully within cache
        memcpy(f->cache + f->head, buff, sz);
        f->dirty = 1;

        f->head += (int)sz;
        if(f->head > f->valid_bytes) {
            f->valid_bytes = f->head;
        }
    } else {
        io300_fetch(f);
        memcpy(f->cache + f->head, buff, sz);
        f->dirty = 1;

        f->head += (int)sz;
        if(f->head > f->valid_bytes) {
            f->valid_bytes = f->head;
        }

    }
    return (ssize_t) sz;
}


int io300_flush(struct io300_file* const f) {
    check_invariants(f);
    if (f->dirty) {
        lseek(f->fd, f->cache_start, SEEK_SET); // setting correct pos
        write(f->fd, f->cache, f->valid_bytes); // writing cache
        f->dirty = 0; // updated
    }
    return 0;
}

int io300_fetch(struct io300_file* const f) {
    check_invariants(f);

    io300_flush(f);
    off_t current_pos = lseek(f->fd, f->cache_start + f->head, SEEK_SET);
    if (current_pos == -1) {
        return -1; 
    }

    memset(f->cache, '\0', CACHE_SIZE); // will get written over by the read
    if(f->head + f->cache_start >= io300_filesize(f)) { // past end of file, to avoid read. 
        f->cache_start += f->head;
        f->head = 0;
        f->valid_bytes = 0;
        return 0;
    } 

    ssize_t bytes_read = read(f->fd, f->cache, CACHE_SIZE);
    if (bytes_read == -1) {
        return -1; 
    }

    f->valid_bytes = (int)bytes_read;
    f->cache_start += f->head;
    f->head = 0; 
    f->dirty = 0; 

    return 0;
}
