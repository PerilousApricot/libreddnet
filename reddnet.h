#ifndef REDDNET_HEADER_H_WHEEEE
#define REDDNET_HEADER_H_WHEEEE

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 28
#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#endif


#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>

// reddnet shim library by Andrew Melo <andrew.m.melo@vanderbilt.edu>
// on error, these functions return -1 and set the error string/long
// which can be extracted by the subsequent functions

#ifdef __cplusplus
  extern "C" {
#endif

int redd_init();
ssize_t redd_read(void * fd, char * buf, ssize_t num);
int redd_close(void * m_fd);
int64_t redd_lseek( void * fd, int64_t pos, uint32_t whence);
void * redd_open (const char *name, int openflags, int perms);
ssize_t redd_write(void * fd, const char * buf, ssize_t num);
int redd_term();

#ifdef __cplusplus
  }
#endif

#ifdef __cplusplus
// error reporting
// these hooks get pulled from c programs, and we need a way to use this include anyway
// FIXME: dirty
#include <string>
extern "C" {
int32_t redd_errno();
const std::string & redd_strerror();
}
#endif

// structures
typedef struct {
	int fd[2];               // set of pipes to communicate back from FUSE
	char * target;           // where to write any string data that comes back
	int64_t target_size;      // the size of the return buffer
} request_t;
typedef struct {
	int is_error;                 // was the response an error?
	int error_number;                     // if so, what was the error
	struct stat file_stat;         // for bringing back stat calls
	struct fuse_entry_param entry; // for bringing back lookup calls
	struct fuse_file_info fi;      // for create calls
	int64_t bytes_written;          // bytes from fuse_write call
	int64_t buffer_length;          // length of additional string data
} response_t;
typedef struct {
	void * fh;
	uint64_t offset;
	uint64_t inode;
} filehandle_t;

typedef struct {
	int retval;
} thread_return_code_t;

#endif
