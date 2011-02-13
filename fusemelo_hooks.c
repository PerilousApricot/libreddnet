#define FUSE_USE_VERSION 28
#include <pthread.h>
#include <stdlib.h>
#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <reddnet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <string.h>
#include <jni.h>


#define LIBPATH   "./:/Users/meloam/ReddNetAdaptor/jFUSE-ll/jFUSE-ll/src/jFUSE/lowlevel/lib/:/Users/meloam/ReddNetAdaptor/jFUSE-ll/jFUSE-ll/src/jFUSE/lowlevel/lib/libjfuselib.dylib"
#define CLASSPATH "./:/Users/meloam/ReddNetAdaptor/BastardFS/src::/Users/meloam/ReddNetAdaptor/jFUSE-ll/jFUSE-ll/src/jFUSE/lowlevel/jFUSE.jar:/Users/meloam/ReddNetAdaptor/lsync/target/lsync-0.0.1-SNAPSHOT.jar:/Users/meloam/ReddNetAdaptor/lStore/client/target/client-1.0.jar:/Users/meloam/ReddNetAdaptor/BastardFS/edu/vanderbilt/accre/bastardfs/bastardfs.jar"
// /Users/meloam/ReddNetAdaptor/BastardFS/src/bastardfs/bastardfs.jar:
//
// Forward declarations
//
void * bootstrap_bfs_real( void * arg );
void * jvm_thread_container( void * arg);

//
// Globals to let us know when to exit
//

pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t count_cv     = PTHREAD_COND_INITIALIZER;
int reference_count         = 0;
int can_exit                = 0;
pthread_mutex_t jvm_mutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t jvm_cv       = PTHREAD_COND_INITIALIZER;
int jvm_started             = 0;
int is_exception            = 1;
pthread_t jvm_thread_handle;
pthread_t bfs_thread_handle;
thread_return_code_t return_code;


pthread_mutex_t ops_mutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ops_cv       = PTHREAD_COND_INITIALIZER;
const struct fuse_lowlevel_ops *op_table = NULL;

//
// linked list to keep track of fds we may need to destroy
// TODO: Ugh, I don't want to do this


// actually blow away the JVM. Wakes up people waiting on various cond_wait
// TODO: I'm going to have to change everything to use a double pointer to
// op_table, that way I can make sure that outstanding requests will die
// right now, a thread can wait forever if it's waiting on a response
// then the JVM is terminated
void terminate_bfs() {

	// Blow away the ops table
	printf("Attempting to lock ops table\n");
	pthread_mutex_lock(&ops_mutex);
	op_table = NULL;
	pthread_cond_broadcast(&ops_cv);
	printf("Erased ops table\n");
	pthread_mutex_unlock(&ops_mutex);

	// Tell everyone else they can go away
	pthread_mutex_lock(&count_mutex);
	can_exit = 1;
	pthread_cond_signal(&count_cv);
	pthread_cond_broadcast(&count_cv);
	pthread_mutex_unlock(&count_mutex);
	printf("BastardFS: Waiting on JVM shutdown\n");
	pthread_join(jvm_thread_handle, NULL);
	printf("BastardFS: JVM has returned\n");
}




// TODO: ADD the mechanisms to pass the paths down to the right layer
// bootstrap_bfs
//     const char * libpath: for JVM
//     const char * classpath: for JVM
// increments internal reference count for the JVM
// and spawns a JVM thread if this is the first one to be initialized
//
int bootstrap_bfs( const char * libpath, const char * classpath ) {
	pthread_mutex_lock(&count_mutex);
	int retval = 0;
	reference_count++;
	can_exit = 0;
	is_exception = 0;
	if (reference_count == 1) {
		if ( ! jvm_started ) {
			/* We were the first call, start the JVM */
			printf("BastardFS: Spawning JVM thread\n");
			retval = pthread_create(&jvm_thread_handle, NULL,
					&jvm_thread_container,
					NULL);
			if ( retval ) {
				perror("BastardFS: Error spawning JVM thread");
				reference_count--;
				is_exception = 1;
				return -1;
			}
		}
		/* Now that the JVM's started, start a new helper thread
		 * to hold BFS
		 */
		printf("BastardFS: Spawning BFS thread\n");
		retval = pthread_create(&bfs_thread_handle, NULL,
				&bootstrap_bfs_real,
				NULL);
		if ( retval ) {
			perror("BastardFS: Error spawning JVM thread");
			reference_count--;
			is_exception = 1;
			return -1;
		}
	}
	pthread_cond_broadcast(&count_cv);
	pthread_mutex_unlock(&count_mutex);
	return retval;
}

void * jvm_thread_container( void * arg ) {
	// Where the JVM will be run permenantly
	/* Declarations */
	static JavaVM 		   *jvm		= NULL;
	static JNIEnv		   *env		= NULL;
	JavaVMOption   options[4];
	JavaVMInitArgs vm_args;
	jclass 		   mainClass = NULL;
	jmethodID 	   mainMethod = NULL;
	jobjectArray   args       = NULL;


	/* Builds the options */
	/* TODO: make this dynamic */
	options[0].optionString = "-Djava.library.path=" LIBPATH;
	options[1].optionString = "-Djava.class.path=" CLASSPATH;
	options[2].optionString = "-Xdebug";
	options[3].optionString = "-Xrunjdwp:transport=dt_socket,address=3408,server=y,suspend=n";
#if 0
	size_t libpath_length, classpath_length;
	int currOption = 0;

	/* First, libpath */
	const char * libpath_prefix = "-Djava.library.path=";
	libpath_length = strlen( libpath_prefix ) + strlen( libpath ) + 1;
	char * libpath_option = (char *) malloc( libpath_length );
	if (libpath_option == NULL) {
		perror("Error in malloc()");
		return -1;
	}
	strlcpy( libpath_option, libpath_prefix, strlen( libpath_prefix ) + 1 );
	strlcpy( libpath_option + strlen(libpath_prefix),
				libpath, strlen( libpath ) + 1);
	if ( strlen( libpath ) ) {
		options[currOption].optionString = libpath_option;
		currOption++;
	}

	/* Now, classpath */
	const char * classpath_prefix = "-Djava.class.path=";
	classpath_length = strlen( classpath_prefix ) + strlen( classpath ) + 1;
	char * classpath_option = (char *) malloc( classpath_length );
	if (classpath_option == NULL) {
		perror("Error in malloc()");
		return -1;
	}

	strlcpy( classpath_option, classpath_prefix, strlen( classpath_prefix ) + 1 );
	strlcpy( classpath_option + strlen(classpath_prefix),
				classpath, strlen( classpath ) + 1);

	if ( strlen( classpath ) ) {
		options[currOption].optionString = classpath_option;
		currOption++;
	}
#endif

	/* Builds the args */
	vm_args.version = 0x00010002;
	vm_args.options = options;
	vm_args.nOptions = 4;
	vm_args.ignoreUnrecognized = JNI_TRUE;


	/* Creates a java jvm only if one's not already started */
	jint error_code;
	if((error_code = JNI_CreateJavaVM(&jvm, (void**)&env, &vm_args)) != 0)
	{
		printf("Unable to start Java VM: %i\n", error_code	);
		goto ERROR_HANDLER;
	}
	jvm_started = 1;

	return NULL;

	/* Error handler */
	ERROR_HANDLER:

		if(jvm != NULL)
		{
			if(env != NULL && (*env)->ExceptionOccurred(env))
			{
				(*env)->ExceptionDescribe(env);
			}

			(*jvm)->DestroyJavaVM(jvm);
		}
		pthread_mutex_lock(&count_mutex);
		is_exception = 1;
		pthread_mutex_unlock(&count_mutex);
		// RACEY
		terminate_bfs( );
		/* Error code */
		//FIXME: get real return codes here
		return NULL;

}

void * bootstrap_bfs_real( void * arg ) {
	/* Declarations */
	static JavaVM 		   *jvm		= NULL;
	static JNIEnv		   *env		= NULL;
	JavaVMOption   options[4];
	JavaVMInitArgs vm_args;
	jclass 		   mainClass = NULL;
	jmethodID 	   mainMethod = NULL;
	jobjectArray   args       = NULL;

	// need to attach the current thread to the JVM
	// TODO: need to detach only on threads we've attached to
	int res = (*jvm)->AttachCurrentThread(jvm, (void **)&env, NULL);
	if (res < 0) {
		printf("Thread attach failed\n");
		goto ERROR_HANDLER;
	}


	/* Gets the main class and method*/
	if (((mainClass = (*env)->FindClass(env, "edu/vanderbilt/accre/bastardfs/BastardFS")) == NULL) ||
		(*env)->ExceptionOccurred(env)) {

		printf("Unable to find BFS class\n");
		goto ERROR_HANDLER;
	}

	if (((mainMethod = (*env)->GetStaticMethodID(env, mainClass, "main",
	                                     "([Ljava/lang/String;)V")) == NULL) ||
	    (*env)->ExceptionOccurred(env)) {
		printf("Unable to find BFS main function\n");
		goto ERROR_HANDLER;
	}


	/* Builds the arg list to the program */
	args = (*env)->NewObjectArray(env, 6, (*env)->FindClass(env, "java/lang/String"),
			(*env)->NewStringUTF(env, "--threads"));

	if(args == NULL)
	{
		printf("HERE3\n");
		goto ERROR_HANDLER;
	}

	/* Sets the elements of the args */
	(*env)->SetObjectArrayElement(env, args, 0,
			(*env)->NewStringUTF(env, "--threads"));
	(*env)->SetObjectArrayElement(env, args, 1,
			(*env)->NewStringUTF(env, "1"));
	(*env)->SetObjectArrayElement(env, args, 2,
			(*env)->NewStringUTF(env, "--configfile"));
	(*env)->SetObjectArrayElement(env, args, 3,
			(*env)->NewStringUTF(env, "bastardfs.properties"));
	(*env)->SetObjectArrayElement(env, args, 4,
			(*env)->NewStringUTF(env, "lstore-gpfs.vampire:/melo-test"));
	(*env)->SetObjectArrayElement(env, args, 5,
			(*env)->NewStringUTF(env, "tmp"));

	/* Calls the main method for the class */
	printf("BastardFS: Starting JVM\n");
	(*env)->CallStaticVoidMethod(env, mainClass, mainMethod, args);
	if(env != NULL && (*env)->ExceptionOccurred(env))
	{
		printf("Error in BastardFS, Java stacktrace: \n");
		goto ERROR_HANDLER;
	}
	printf("BastardFS: Shutting down\n");

	/* Frees the jvm */
	// NOTE: Don't do this. You can't start it back up (stupid java)
	//(*jvm)->DestroyJavaVM(jvm);

	/* Return success */
	return EXIT_SUCCESS;


	/* Error handler */
	ERROR_HANDLER:

		if(jvm != NULL)
		{
			if(env != NULL && (*env)->ExceptionOccurred(env))
			{
				(*env)->ExceptionDescribe(env);
			}

			(*jvm)->DestroyJavaVM(jvm);
		}
		pthread_mutex_lock(&count_mutex);
		is_exception = 1;
		pthread_mutex_unlock(&count_mutex);
		// RACEY
		terminate_bfs( );
		/* Error code */
		//FIXME: get real return codes here
		return NULL;
 }
int destroy_bfs(void ** return_target) {

	pthread_mutex_lock(&count_mutex);
	printf("Decrementing reference count\n");
	reference_count--;
	if ( reference_count < 0 ) {
		printf("BastardFS: ERROR! reference_count is negative!\n");
		reference_count = 0;
		pthread_mutex_unlock(&count_mutex);
	}

	/* may have fixed a negative reference_count earlier */
	if ( reference_count == 0) {
		pthread_mutex_unlock(&count_mutex);
		terminate_bfs();

		return 0;
	} else {
		pthread_mutex_unlock(&count_mutex);
		// FIXME: add some error handling
		return 0;
	}
}

const struct fuse_lowlevel_ops * get_op_table() {
	const struct fuse_lowlevel_ops * my_copy = NULL;
	pthread_mutex_lock(&ops_mutex);
	printf("Ops mutex, bitchazz\n");
	while ( op_table == NULL ) {
		pthread_cond_wait(&ops_cv, &ops_mutex);
		if ( is_exception ) {
			// something blew up, panic and quit
			printf("BastardFS: An error occured while pulling up the ops table. Quitting\n");
			pthread_mutex_unlock(&count_mutex);
			return NULL;
		}
	}
	my_copy = op_table;
	printf("Got it!\n");
	pthread_mutex_unlock(&ops_mutex);
	return op_table;
}

// replies w/payload
int send_response_with_payload( fuse_req_t req, response_t * res, const char * buf , size_t size ) {
	res->buffer_length = size;
	ssize_t bytes_written = write( ((request_t* ) req)->fd[1], (char *) res, sizeof(response_t));
	if ( bytes_written != sizeof(response_t) ) {
		return -1;
	}
	if ( buf != NULL ) {
		bytes_written = write( ((request_t*) req)->fd[1], buf, size) ;
		if ( bytes_written != size ) {
			return errno;
		}
	}
	return 0;
}
// send w/o payload
int send_response( fuse_req_t req, response_t * res ) {
	res->buffer_length = 0;
	ssize_t bytes_written = write( ((request_t* ) req)->fd[1], (char *) res, sizeof(response_t));
	if ( bytes_written != sizeof(response_t) ) {
		return -1;
	}
	return 0;
}
int fuse_reply_write( fuse_req_t req, size_t count ) {
	response_t res;
	res.bytes_written = count;
	res.is_error = 0;
	return send_response( req, &res );
}
int fuse_reply_open	(	fuse_req_t 	req,const struct fuse_file_info * 	fi) {
	response_t res;
	res.is_error = 0;
	return send_response( req, &res );
}
int fuse_reply_err	(	fuse_req_t 	req, int 	err	 ) {
	printf( "Hit fusre_reply_error\n");
	printf( "Error number is %i\n", err);
	response_t res;
	res.error_number = err;
	res.is_error = 1;
	res.entry.ino = 0;
	return send_response( req, &res );

}
int fuse_reply_statfs( fuse_req_t req,const struct statvfs * stbuf ) {
	response_t res;
	memcpy( &res.file_stat,
			stbuf,
			sizeof( struct stat ));
	res.is_error = 0;
	return send_response( req, &res );
}
int fuse_reply_buf(	fuse_req_t req, const char * buf, size_t size )	{
	response_t res;
	res.is_error = 0;
	return send_response_with_payload( req , &res, buf, size);
}
void fuse_reply_none (fuse_req_t req) {
	response_t res;
	res.is_error = 0;
	send_response( req, &res );
}
int fuse_reply_create (fuse_req_t req, const struct fuse_entry_param *e, const struct fuse_file_info *fi){
	response_t res;
	res.is_error = 0;
	memcpy( &res.entry,
			e,
			sizeof (struct fuse_entry_param));
	memcpy( &res.fi,
			fi,
			sizeof (struct fuse_file_info));

	return send_response( req, &res );
}
int fuse_reply_attr (fuse_req_t req, const struct stat *attr, double attr_timeout) {
	response_t res;
	res.is_error = 0;
	memcpy( &res.file_stat,
			attr,
			sizeof( struct stat ));

	return send_response( req, &res );
}
int fuse_reply_entry (fuse_req_t req, const struct fuse_entry_param *e) {
	response_t res;
	res.is_error = 0;
	memcpy( &res.entry,
			e,
			sizeof (struct fuse_entry_param));
	return send_response( req, &res );

}



int fuse_session_loop (struct fuse_session *se) {
	pthread_mutex_lock(&count_mutex);
	printf("Entering session loop\n");
	while ( !can_exit ) {
		printf("Looping over session\n");
		pthread_cond_wait(&count_cv, &count_mutex);
		printf("Looping over session -- return from condition\n");
	}
	pthread_mutex_unlock(&count_mutex);
	printf("Shutting down ReDDNet...\n");
	return 0;
}

int fuse_session_loop_mt (struct fuse_session *se) {
	pthread_mutex_lock(&count_mutex);
	while ( !can_exit ) {
		pthread_cond_wait(&count_cv, &count_mutex);
	}
	pthread_mutex_unlock(&count_mutex);
	pthread_exit(NULL);
	printf("Shutting down ReDDNet...\n");
	return 0;
}


struct fuse_session * 	fuse_lowlevel_new (struct fuse_args *args, const struct fuse_lowlevel_ops *op, size_t op_size, void *userdata) {
	printf("trying to set ops table\n");
	pthread_mutex_lock(&ops_mutex);
	printf("Ops table set\n");
	op_table = op;
	pthread_cond_broadcast(&ops_cv);
	pthread_mutex_unlock(&ops_mutex);
	// need to return a dummy pointer. It's opaque to fuse
	// but needs to be non-null
	return (struct fuse_session *) 1;
}
struct fuse_ctx dummyCtx;
const struct fuse_ctx *fuse_req_ctx(fuse_req_t req) {
	// NULL is for exceptional states, we want jFUSE to ignore
	// this call. Give it a non-null pointer (they're opaque to jFUSE
	// anyway)
	dummyCtx.pid = 2;
	dummyCtx.gid = 2;
	dummyCtx.uid = 2;
	return &dummyCtx;
}

struct fuse_chan * 	fuse_mount (const char *mountpoint, struct fuse_args *args) {
	// NULL is for exceptional states, we want jFUSE to ignore
	// this call. Give it a non-null pointer (they're opaque to jFUSE
	// anyway)
	printf("BastardFS: fuse_mount called\n");
	return (struct fuse_chan *) 1;
}

void 	fuse_session_add_chan (struct fuse_session *se, struct fuse_chan *ch) {
	// do nothing
}

void 	fuse_unmount (const char *mountpoint, struct fuse_chan *ch) {
	// do nothing
	printf("BastardFS: fuse_unmount called, we must be exiting\n");
}


