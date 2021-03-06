#include "http-client.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <iostream>

using namespace std;

struct progress {
	double lastruntime;
    curl_off_t curr_upload;
    curl_off_t upload_max;
};

static struct progress curl_progress;

static CURLM *cm;
static CURL *curl;

static FILE *curr_fd;

static string local_fn;
static string remote_path;

static int remote_port;
static string remote_base_url;
static string full_remote_path;

static bool in_progress = false;
static int cancel_flag;
static pthread_mutex_t send_lock;

static pthread_mutex_t error_lock;
static string curl_error_string;

void *send_worker(void*);
int info_callback(void *p, curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t ultotal, curl_off_t ulnow);
int set_global_opts();
int init_file_upload(curl_off_t f_offset, curl_off_t numBytes);
int create_full_path();
void update_error_string(const char *err);

int asynch_send(const char *filename, curl_off_t f_offset, curl_off_t numBytes, const char *rem_path) {
    // Check to make sure more than one simultaneous transfer doesn't occur
    pthread_mutex_lock(&send_lock);

    if(in_progress) {
        pthread_mutex_unlock(&send_lock);
        update_error_string("Attempted to start second simultaneous transfer\n");
        cerr << "Transfer already in progress, canceling." << endl;
        return -1;
    }
    
    local_fn = string(filename);
    remote_path = string(rem_path);

    // Initialize global curl easy struct
    if (init_file_upload(f_offset, numBytes) < 0) {
        cerr << "Init file upload failed." << endl;

        in_progress = false;
        pthread_mutex_unlock(&send_lock);

        return -1;
    }

    // Add single handle to multi
    curl_multi_add_handle(cm, curl); 

    pthread_t id;
    // Set thread to detached
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    in_progress = true;
    pthread_mutex_unlock(&send_lock);

    cout << "Spawning upload worker." << endl;

    // Create thread
    pthread_create(&id, &attr, send_worker, NULL);  

    return 0; 
}

/*
    REQUIRES:
        asynch_send must create thread to run send_worker
        arg, cast to req_info struct. Contains local and remote path info

    MODIFIES:
        cm, through curl_multi add and remove handle. Reentrant
        in_progress, cancel_flag for flagging transfer status
        curl, curr_fd, full_remote_path through init_file_upload
    
    RETURNS:
        -1 if exiting prematurely. Detatched thread, returns will not be received
*/
void *send_worker(void*) {
    CURLMsg *msg=NULL;
    CURLcode res;
    int still_running=0, msgs_left=0;
    double speed_upload, total_time;
    
    printf("Executing upload worker.\n");

    curl_multi_perform(cm, &still_running);

    // Perform request
    // Execute other code inside body
    do {
        int numfds=0;
        int res_code = curl_multi_wait(cm, NULL, 0, MAX_WAIT_MSECS, &numfds);
        pthread_mutex_lock(&send_lock);
        if(res_code != CURLM_OK && cancel_flag != 1) {
            pthread_mutex_unlock(&send_lock);
            update_error_string("Curl_multi_wait() error\n");
            
            // Removing handle mid-transfer will abort request
            curl_multi_remove_handle(cm, curl);
            
            // Cleanup
            fclose(curr_fd);
            pthread_mutex_lock(&send_lock);
            in_progress = false;
            cancel_flag = 0;
            pthread_mutex_unlock(&send_lock);
            return (void*)-1;
        }
        pthread_mutex_unlock(&send_lock);
        curl_multi_perform(cm, &still_running);

    } while(still_running);
    while ((msg = curl_multi_info_read(cm, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            curl = msg->easy_handle;

            res = msg->data.result;
            if(res != CURLE_OK) {
                fprintf(stderr, "CURL error code: %d\n", msg->data.result);
                continue;
            }
            else {
                // now extract transfer info
                curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD, &speed_upload);
                curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_time);

                fprintf(stderr, "Speed: %.3f bytes/sec during %.3f seconds\n",
                      speed_upload, total_time);
            }
        }
        else {
            update_error_string("Error after curl_multi_info_read()\n");
        }
    }
    // Cleanup
    curl_multi_remove_handle(cm, curl);
    
    fclose(curr_fd);
    pthread_mutex_lock(&send_lock);
    in_progress = false;
    cancel_flag = 0;
    pthread_mutex_unlock(&send_lock);

    printf("thread done\n");

    pthread_exit(NULL);
}

int curl_init(const char *host, long port) {
    remote_base_url = string(host);
    remote_port = port;

    // Realloc treats null pointer as normal malloc
    //full_remote_path = string("");
    
    // Create empty error string
    curl_error_string = string("No error");

    // Global curl initialization
    // Not thread safe, must be called once
    curl_global_init(CURL_GLOBAL_ALL);

    curl = curl_easy_init();
    if (curl == NULL) {
        update_error_string("Curl easy_init failed\n");
        return -1;
    }
    set_global_opts();

    // Initialize multi, single
    cm = curl_multi_init();
    if (cm == NULL) {
        update_error_string("Curl multi_init failed\n");
        return -1;
    }
    
    curl_progress.lastruntime = 0;
    curl_progress.curr_upload = 0;
    curl_progress.upload_max = 0;

    // lock initialization
    pthread_mutex_init(&send_lock, NULL);
    pthread_mutex_init(&error_lock, NULL);

    // Thread flag initialization
    in_progress = false;
    cancel_flag = 0;

    return 0;
}

int curl_destroy() {
    pthread_mutex_lock(&send_lock);
    if (in_progress) {
        update_error_string("Cannot destroy module, transfer in progress\n");
        return -1;
    }
    pthread_mutex_unlock(&send_lock);
    
    pthread_mutex_destroy(&send_lock);
    pthread_mutex_destroy(&error_lock);

    curl_easy_cleanup(curl);
    curl_multi_cleanup(cm);

    return 0;
}

curl_off_t status_send() { 
    curl_off_t ret = -1;
    pthread_mutex_lock(&send_lock);
    if (in_progress) {
        ret = curl_progress.curr_upload;
    }
    pthread_mutex_unlock(&send_lock);
    return ret;
}

int cancel_send() {
    pthread_mutex_lock(&send_lock);
    if (in_progress)
        cancel_flag = 1;
    pthread_mutex_unlock(&send_lock);
    return 0;
}

// Callback structure progressfunc.c example
/*
    REQUIRES:
        Successful curl_init
        Transfer to be in progress (called as callback to multi perform)

    MODIFIES:
        curl_progress.lastruntime
        curl_progress.curr_upload

    RETURNS:
        0 on success
*/
int info_callback(void *p, curl_off_t dltotal, curl_off_t dlnow,
                  curl_off_t ultotal, curl_off_t ulnow) {
    // Recast struct to progress
    struct progress *myp = (struct progress *)p;
    double curtime = 0;

    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &curtime);

    /* under certain circumstances it may be desirable for certain functionality
     to only run every N seconds, in order to do this the transaction time can
     be used */ 
    if ((curtime - myp->lastruntime) >= MINIMAL_PROGRESS_FUNCTIONALITY_INTERVAL) {
        myp->lastruntime = curtime;
        fprintf(stderr, "TOTAL TIME: %f \r\n", curtime);
    }
    
    pthread_mutex_lock(&send_lock);
    myp->curr_upload = ulnow;
    pthread_mutex_unlock(&send_lock);
       
    int debug = 0;
    if (debug) {
        fprintf(stderr, "UP: %" CURL_FORMAT_CURL_OFF_T " of %" CURL_FORMAT_CURL_OFF_T
              "  DOWN: %" CURL_FORMAT_CURL_OFF_T " of %" CURL_FORMAT_CURL_OFF_T
              "\r\n",
              ulnow, ultotal, dlnow, dltotal); 
    }
   
    return 0;
}

/*
    REQUIRES:
        Valid (initialized) pointer to curl struct
        port number of server

    MODIFIES:
        curl

    RETURNS:
        0 on success
*/
int set_global_opts() { 
    /* upload to this place */ 
    curl_easy_setopt(curl, CURLOPT_PORT, remote_port);
    /* tell it to "upload" to the URL */ 
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_PUT, 1L);

    // Progress callback options
 	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, info_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &curl_progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    /* enable verbose for easier tracing */ 
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    return 0;
}

/*
    REQUIRES:
        Valid (initialized) pointer to curl struct
        curl struct pointer, local_fn, remote_path, full_remote_path
        file offset argument

    MODIFIES:
        curr_fd, curl struct, full_remote_path (through create_full_path)
        curl_progress.upload_max

    RETURNS:
        0 on success
        -1 on bad file open, offset greater than file size
*/
int init_file_upload(curl_off_t f_offset, curl_off_t numBytes) {
	full_remote_path = remote_base_url + remote_path;

	printf("Full remote: %s\n", full_remote_path.c_str());
    printf("Local filename is %s\n", local_fn.c_str());

    curr_fd = fopen(local_fn.c_str(), "rb"); /* open file to upload */

    if(!curr_fd) {
        update_error_string("failed to open local file\n");
        return -1; /* can't continue */
    }

    // Get file size
    fseek(curr_fd, 0L, SEEK_END);
    curl_off_t sz = ftell(curr_fd);

    // Seek to offset, if offset small enough
    if(sz < f_offset || fseek(curr_fd, f_offset, SEEK_SET) != 0) {
        update_error_string("Fseek failed or offset beyond file size\n");
        return -1;
    }

    printf("Full remote path is: %s\n", full_remote_path.c_str());
    printf("Size is: %lu\n", (unsigned long)(sz - f_offset));

    curl_easy_setopt(curl, CURLOPT_URL, full_remote_path.c_str());
    /* set where to read from */ 
    curl_easy_setopt(curl, CURLOPT_READDATA, curr_fd);
    /* and give the size of the upload (optional) */ 
    if(numBytes > (sz-f_offset)) {
    	return -1;
    }
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, numBytes);

    // Set upload max value from file size
    // We already acquired a lock by this point, no need to over complicate.
    //pthread_mutex_lock(&send_lock);
    curl_progress.upload_max = (curl_off_t)sz;
    //pthread_mutex_unlock(&send_lock);
    return 0;
}

/*
    REQUIRES:
        successful completion of curl_init (for remote base url)
        remote_base_url, remote_path, full_remote_path

    MODIFIES:
        full_remote_path

    RETURNS:
        0 on success
        -1 for bad realloc
*/
int create_full_path() {

    return 0;
}

string get_error_string() {
    string ret;
    pthread_mutex_lock(&error_lock);
    ret = curl_error_string;
    pthread_mutex_unlock(&error_lock);

    return ret;
}

/*
    REQUIRES:
        successful completion of curl_init (for error_lock)
        null terminated error string as arg

    MODIFIES:
        curl_error_string, thread safe

    RETURNS:
        void
*/
void update_error_string(const char *err) {
    pthread_mutex_lock(&error_lock);
    curl_error_string = string(err);
    pthread_mutex_unlock(&error_lock);
}
