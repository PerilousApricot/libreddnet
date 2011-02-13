#include <sys/types.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <exception>
#include <stdexcept>
#include <iostream>
#include <pthread.h>
#include <map>
#include <list>
#include <fcntl.h>
#include <errno.h>
#define FUSE_USE_VERSION 28
#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include "reddnet.h"


// reddnet shim library by Andrew Melo <andrew.m.melo@vanderbilt.edu>
int private_redd_errno = 0;
std::string private_redd_errstr = "No Error";
void * library_handle;
bool is_loaded = false;
pthread_t waiting_thread;
const struct fuse_lowlevel_ops * ops_table;
std::map<unsigned long, ssize_t> offset_lookup;
int (*bootstrap_bfs)(const char *, const char *) = NULL;
int (*destroy_bfs)(void *) = NULL;
const struct fuse_lowlevel_ops * (*get_op_table)() = NULL;
pthread_t jvm_thread;
pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

// forward declarations

void * generate_filehandle( unsigned long inode );
void destroy_filehandle( void * fh );
off_t get_offset( void * fh );
void set_offset( void * fh , off_t offset);
unsigned long get_inode( void * fh );
struct fuse_file_info generate_fileinfo( void * fh );
int init_request( request_t * request_object );
response_t  wait_request( request_t * request_object, int * return_code );
void throw_redd_error( std::string complaint, int my_error = 1 );
void clear_errors();
void * boot_jvm_thread(void * arg);

//
// Entry point from CMSSW
//
// Returns 0 on success, -1 on error

int redd_init() {
	pthread_mutex_lock(&init_mutex);
	clear_errors();
	// until ACCRE removes the java dependency from their client libs,
	// we'll dlopen() them so they don't need to be brought along with cmssw
	// if you're running ReDDNet at your site, you will have the libs anyway
	library_handle =
	     dlopen("/Users/meloam/ReddNetAdaptor/jFUSE-ll/jFUSE-ll/src/jFUSE/lowlevel/lib/libjfuselib.dylib", RTLD_LAZY);
	if (library_handle == NULL) {
		throw_redd_error(std::string("The ReddNet libraries can't be found: ") +
							dlerror());
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}

	// We need to pull in bootstrap_bfs and destroy_bfs from the shared
	// library.
	char * retval;
	/* get bootstrap_bfs */
	dlerror();
	bootstrap_bfs = (int (*)(const char *, const char *))dlsym(library_handle, "bootstrap_bfs");
	if ( retval = dlerror() ) {
		throw_redd_error(std::string("Failed to load dlsym ReDDNet library: ") + retval);
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}
	if (bootstrap_bfs == NULL) {
		throw_redd_error("Got a null pointer back from dlsym()\n");
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}
	/* get destroy_bfs */
	dlerror();
	destroy_bfs = (int (*)(void *))dlsym(library_handle, "destroy_bfs");
	if ( retval = dlerror() ) {
		throw_redd_error(std::string("Failed to load dlsym ReDDNet library: ") + retval);
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}
	if (destroy_bfs == NULL) {
		throw_redd_error("Got a null pointer back from dlsym()\n");
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}

	/* get get_op_table */
	dlerror();
	get_op_table = (const struct fuse_lowlevel_ops * (*)())dlsym(library_handle, "get_op_table");
	if ( retval = dlerror() ) {
		throw_redd_error(std::string("Failed to load dlsym ReDDNet library: ") + retval);
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}
	if (get_op_table == NULL) {
		throw_redd_error("Got a null pointer back from dlsym()\n");
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}

	if ( (*bootstrap_bfs)("dummy till I fix classpath", "ditto") ) {
		printf("Failed to boostrap ReDDNet library:");
		throw_redd_error(std::string("Failed to spawn JVM thread"));
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}

	ops_table = (*get_op_table)();
	if ( ops_table == NULL ) {
		throw_redd_error(std::string("ReDDNet terminated before ops"));
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}
	is_loaded = true;
	pthread_mutex_unlock(&init_mutex);
	return 0;
}

//
// Terminates the library. Internally, destroy_bfs decrements a
// reference count and only shuts down if there is no more refs
// when that happens, all outstanding fs ops will be terminated
//
// returns 0 on success and -1 on (currently unknown) errors
int redd_term() {
	pthread_mutex_lock(&init_mutex);
	clear_errors();
	(*destroy_bfs)(NULL);

	// unsure if it's a big deal to leave these handles around
	/*bootstrap_bfs = NULL;
	destroy_bfs   = NULL;
	get_op_table  = NULL;
	if (dlclose(library_handle)) {
		throw_redd_error("Couldn't unload shared library");
		pthread_mutex_unlock(&init_mutex);
		return -1;
	}*/
	is_loaded = false;
	pthread_mutex_unlock(&init_mutex);
	return 0;
}

//
// Accessors for error codes
//

long redd_errno(){
	return private_redd_errno;
}

const std::string & redd_strerror(long unused){
	return private_redd_errstr;
}

//
// Opens a file
// returns NULL on error
//

void * redd_open (const char * name,int openflags,int perms) {
	clear_errors();
	int return_code = 0;
	std::string name_string( name );
	request_t request_object;
	response_t response;
	unsigned long inode = 1;
	int slash_pos = 0;
	bool keep_going = true;

	// have to walk up the directory path to get the inode of the target

	// TODO: need to handle relative addresses. Not even sure of how that works
	// for now, we need to make sure to eat a leading slash if it's there, but assume
	// the path is absolute for now
	if ( name_string[0] == '/' ) {
		slash_pos = 1;
	}

	while (keep_going) {
		size_t next_slash = name_string.find("/", slash_pos);
		if ( next_slash == std::string::npos ) {
			keep_going = false;
			break;
		}
		init_request( &request_object );
		const char * substr = name_string.substr( slash_pos + 1, next_slash - slash_pos -1 ).c_str();
		(ops_table->lookup)( (fuse_req_t) &request_object, 
					inode, 
					(name_string.substr( slash_pos + 1, next_slash - slash_pos - 1 ).c_str())
				);
		response = wait_request( &request_object, &return_code );
		// error handling
		if (return_code && (response.error_number != ENOENT)) {
			return NULL;
		}
		
		// the specified directory doesn't exist, create it
		// ASSUMPTION: CMSSW doesn't make directories for LFNs
		// so we need to
		if ( response.error_number == ENOENT ) {
			if ( ! (openflags & O_CREAT ) ) {
				// we didn't ask to create a file, so don't make the dirs
				throw_redd_error( "File not found, and we weren't asked to make one");
				return NULL;
			}
			init_request( &request_object );
			(ops_table->mkdir)( (fuse_req_t) & request_object,
									inode,
									name_string.substr( slash_pos + 1, next_slash - slash_pos - 1 ).c_str(),
									perms);
			response = wait_request( &request_object, &return_code );
			// error handling
			if (return_code) { return NULL; }
		}
		
		inode = response.entry.ino;
		slash_pos = next_slash;
	}

	// we've searched the directories, now see if the actual file exists
	init_request( &request_object );
	(ops_table->lookup)( (fuse_req_t) &request_object, 
				inode, 
				(name_string.substr( slash_pos ).c_str()));
	response = wait_request( &request_object, &return_code );
	if (return_code && (response.error_number != ENOENT)) {
		return NULL;
	}
	
	void * fh;
	printf("Looking for inodes\n");
	if ( response.entry.ino != 0 ) {
		// we have the inode, open it
		inode = response.entry.ino;
		init_request( &request_object );
		fh = generate_filehandle( inode );
		struct fuse_file_info fi;
		fi.fh = (uint64_t) fh;
		fi.flags = openflags;
		(ops_table->open)( (fuse_req_t) &request_object,
						inode,
						&fi);
		wait_request( &request_object, &return_code );
		// error handling
		if (return_code) { return NULL; }
	} else if ( ( response.entry.ino == 0 ) &&
				( openflags & O_CREAT ) ){
		// file doesn't exist, and we want to make it
		init_request( &request_object );
		fh = generate_filehandle( inode );
		struct fuse_file_info fi;
		fi.flags = openflags;
		fi.fh = (uint64_t) fh;
		(ops_table->create)( (fuse_req_t) &request_object,
						inode,
						name_string.substr( slash_pos ).c_str(),
						perms,
						&fi);
		response = wait_request( &request_object, &return_code );
		// error handling
		if (return_code) { return NULL; }
		((filehandle_t *) fh)->inode = response.entry.ino;
	} else if ( ( response.entry.ino == 0 ) &&
			   !( openflags & O_CREAT ) ) {
		// file didn't exist, and the create flag isn't there
		throw_redd_error("File Not Found, and we weren't told to make it");
		return NULL;
	}
	return fh;
}

//
// Reads from a given filehandle, pushes the offset forward
// returns -1 on error
//

ssize_t redd_read(void * fh, char * buf, ssize_t num) {
	clear_errors();
	int return_code = 0;

	// Valid replies: fuse_reply_buf fuse_reply_err
	// void(* fuse_lowlevel_ops::read)(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
	struct fuse_file_info fi = generate_fileinfo( fh );
	request_t request_object;
	init_request( &request_object );
	request_object.target_size = num;
	request_object.target = buf;
	(*ops_table->read)( (fuse_req_t) &request_object, get_inode(fh), num, get_offset(fh), &fi );
	response_t response = wait_request( &request_object, &return_code );
	// error handling
	if (return_code) { return -1; }
	set_offset( fh, response.buffer_length );
	return response.buffer_length;
}

//
// Closes filehandle
// returns -1 on error
//

int redd_close(void * fh) {
	clear_errors();
	int return_code = 0;

	request_t request_object;
	struct fuse_file_info fi = generate_fileinfo( fh );
	init_request( &request_object );
	(*ops_table->release)( (fuse_req_t) &request_object, get_inode(fh), &fi );
	response_t response = wait_request( &request_object, &return_code );
	// error handling
	if (return_code && (response.error_number != 0)) { return -1; }
	return 0;
}

//
// Seeks to a position in the file
// returns -1 on error

off_t redd_lseek( void * fh, off_t offset, int which ) {
	clear_errors();
	int return_code = 0;

	// need to get the filesize to do some sanity checking
	request_t request_object;
	struct fuse_file_info fi = generate_fileinfo( fh );
	init_request( &request_object );
	//void(* fuse_lowlevel_ops::getattr)(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
	(*ops_table->getattr)((fuse_req_t) &request_object, get_inode(fh), &fi);
	response_t response = wait_request( &request_object, &return_code );
	// error handling
	if (return_code) { return -1; }
	off_t filesize   = response.file_stat.st_size;
	off_t target_pos = 0;
	if ( which == SEEK_SET ) {
		target_pos = offset;
	} else if ( which == SEEK_CUR ) {
		target_pos = get_offset( fh ) + offset;
	} else if ( which == SEEK_END ) {
		target_pos = filesize + offset;
	} else {
		throw_redd_error( "Invalid seek direction" );
		return -1;
	}
	
	if ( target_pos < 0 ) {
		throw_redd_error( "Attempted to seek to a negative position" );
		return -1;
	}
	set_offset( fh, target_pos );
	return target_pos;
}

//
// writes to filehandle
// returns -1 on error, bytes written otherwise


ssize_t redd_write(void * fh, const char * buf, ssize_t num) {
	clear_errors();
	int return_code = 0;

	request_t request_object;
	struct fuse_file_info fi = generate_fileinfo( fh );
	init_request( &request_object );
	(*ops_table->write)( (fuse_req_t) &request_object, get_inode(fh), buf, num, get_offset( fh ), &fi );
	response_t response = wait_request( &request_object, &return_code );
	// error handling
	if (return_code) { return -1; }
	return response.bytes_written;
}

//
// Request handling functions, how we get to/from FUSE
//

int init_request( request_t * request_object ) {
	if ( ! is_loaded ) {
		throw_redd_error("ReddNet libraries are not loaded, did you call redd_init()?");
		return -1;
	}
	if ( pipe( request_object->fd ) == -1 ) {
		throw_redd_error("Couldn't make pipe");
		return -1;
	}
	return 0;
}

// this code is backwards because of removing c++ exception handling
response_t  wait_request( request_t * request_object, int * return_code ) {
	// wait for the reply to come back
	(*return_code) = 0;
	response_t response;
	if ( read( request_object->fd[0], (char *) &response, sizeof(response_t) ) 
			!= sizeof(response_t) ) {
		(*return_code) = 1;
		throw_redd_error("Error in response, got a short read. Might be due to shutdown");
		return response;
	}

	// check for an error
	if ( response.is_error ) {
		(*return_code) = 1;
		// FIXME: SUPER non-threadsafe
		throw_redd_error(std::string("Error in reddnet response") + strerror(response.error_number));
	}

	// pull out any string information
	if ( ( response.buffer_length != 0   ) &&
		 ( request_object->target != NULL ) &&
		 ( request_object->target_size !=0 ) ){
		ssize_t read_bytes = 
			read( request_object->fd[0], request_object->target, 
				(response.buffer_length < request_object->target_size) ?
					response.buffer_length :
					request_object->target_size);
		if ( read_bytes < response.buffer_length ) {
			throw_redd_error("Less data was in the pipe than we were told", errno);
			(*return_code) = 1;
			return response;
		}
		
		if ( read_bytes < 0 ) {
			throw_redd_error("Error reading back bytestring from reddnet", errno);
			(*return_code) = 1;
			return response;
		}
	}
	// get some cleanup
	close( request_object->fd[0] );
	close( request_object->fd[1] );
	return response;
}


//
// Filehandle manipulation functions
//
void * generate_filehandle( unsigned long inode ) {
	filehandle_t * retval = (filehandle_t *) malloc( sizeof( filehandle_t ) );
	if ( retval == NULL ) {
		throw_redd_error("No more ram for filehandles");
	}
	retval->fh     = (void *) retval;
	retval->offset = 0;
	retval->inode  = inode;
	return (void *) retval;
}


void destroy_filehandle( void * fh ) {
	free( fh );
}


off_t get_offset( void * fh ){
	return ((filehandle_t *) fh)->offset;
}


void set_offset( void * fh , off_t offset) {
	((filehandle_t *) fh)->offset += offset;
}


unsigned long get_inode( void * fh ) {
	return ((filehandle_t *) fh)->inode;
}


struct fuse_file_info generate_fileinfo( void * fh ) {
	fuse_file_info fi;
	fi.fh = (uint64_t) fh;
	return fi;
}

//
// Error handling
//

void throw_redd_error( std::string complaint, int error_number ) {
	private_redd_errno  = error_number;
	private_redd_errstr = complaint;
}


void clear_errors() {
	private_redd_errno  = 0;
	private_redd_errstr = "No Error";
}

//
// Testing
//

