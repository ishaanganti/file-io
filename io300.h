#ifndef IO300_H
#define IO300_H

#include <sys/types.h>
struct io300_file;

struct io300_file* io300_open(const char* path, char* description);
int io300_close(struct io300_file* f);
off_t io300_filesize(struct io300_file* f);
int io300_seek(struct io300_file* f, off_t pos);
int io300_readc(struct io300_file* f);
int io300_writec(struct io300_file* f, int ch);
ssize_t io300_read(struct io300_file* f, char* buff, size_t nbytes);
ssize_t io300_write(struct io300_file* f, const char* buff, size_t nbytes);
int io300_flush(struct io300_file* f);

#endif
