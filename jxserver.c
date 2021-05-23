#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <poll.h>
#include "portable_endian.h"
#include <semaphore.h>
#include <dirent.h>
#include <assert.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include "performance-parameters.h"


#define MAX_BUF_LEN 1024
#define NUM_SEGS 256
#define NUM_THREADS CPUS
#define RETRIEVE_HEADER 20

// Tree nodes
struct node {
    
    // Length of compression encoding
    uint8_t length;
    // Compressed encoding
    uint32_t encoding;
    // Original byte 
    uint8_t symbol;
    // An array of children pointers
    struct node** children;
    // Pointer to parent
    struct node* parent;


}; 

// Session node
struct session {

    uint32_t sessionID;

    uint64_t start_offset;

    uint64_t length;

    char* filename;

    char* filepath;
    
    // Number of segments being handled / finished handling
    size_t handled_segments;
    // Number of segments actually sent
    size_t sent_segments;

    // semaphore to lock handled segments to ensure
    // each segment is only handled once
    sem_t seg_lock;
    // Locks the sent_segments variable
    sem_t sent_lock;
    // Session payload
    uint8_t* payload;
    // Pointer to previous session node in linked list
    struct session* prev;
    // Pointer to next session node in linked list
    struct session* next;
    // Indicator of whether this sessino has been completed
    uint8_t completed;

};

/*
struct used to split and store client request into its components
*/
struct data_packet {

    // socket file descriptor
    int socket_fd;

    // Message header (Used for creating response messages)
    uint8_t header;

    // Type Digit (Only used for internal operations)
    uint8_t type;
    // Compression bit: Indicates whether the payload is compressed
    uint8_t compress;
    // Require compression bit: Whether the response requires compression
    uint8_t req_compress;
    // Payload length
    uint64_t payload_len;
    // Payload
    uint8_t* payload;
    
    // MARK: Variables for dynamic payload length
    uint64_t payload_cap;

    // Directory
    char* directory;

    // compression array
    struct node* compress_arr;
    // decompression tree
    struct node* decompress_tree;

    // Number of connections
    size_t* num_packets;
    // Pointer to an array of all connections
    struct data_packet*** packets;

    // Pointer to the linked list of sessions
    struct session** sessions;

    // Pointer to session lock shared by all threads
    sem_t* session_lock;

    // Epoll file descriptor
    int epfd;

};

// MARK: Bit manipulation functions

// Test for the binary value at position bit
uint8_t bit_test(uint8_t bit, uint8_t byte){

    bit = 1 << bit;

    if(bit & byte){
        return 1;
    } else {
        return 0;
    }


}

// Write width bits into position pos of array buf
void bit_write(uint8_t* buf, size_t pos, uint64_t encoding, int width){

    // Check that the width doesn't exceed maximum supported encoding length
    assert(width >= 0 && width <= 64 - 7);

    uint64_t modded_encode = encoding;

    // Bitshift the desired encoding to where the previous part finished
    modded_encode <<= (64 - width - (pos % 8));

    // Turn into big endian for payload
    uint64_t be_bytes = htobe64(modded_encode);

    // Perform OR operation to write it into the buf
    *((uint64_t*)(&buf[pos / 8])) |= be_bytes;


}

// Extracts width bits from positino pos in buf
uint64_t bit_extract(const char* buf, size_t pos, int width){

    // Check that the width doesn't exceed 8 bytes
    assert(width >= 0 && width <= 64 - 7);

    // Grab the 8 bytes that the pos belongs within
    uint64_t bits = *((uint64_t*)(&buf[pos / 8]));

    // Convert it into big endian
    bits = htobe64(bits);

    // Left shift to zero out the bits on the left
    bits <<= pos % 8;
    
    // Then right shift all the way bakc to the right for the value
    return bits >> (64 - width);

}

/*
Determine type of request given the header byte
*/ 
int determ_type(uint8_t header){

    uint8_t total = 0;
    uint8_t bit = 1 << 7;
    total += (bit & header);
    bit = 1 << 6;
    total += (bit & header);
    bit = 1 << 5;
    total += (bit & header);
    bit = 1 << 4;
    total += (bit & header);

    total /= 16;

    return total;

}

/*
Given a type of response, update the header byte inside the packet
*/
void set_type(struct data_packet* packet, uint8_t type, uint8_t compressed){

    // new header that will undergo modifications
    uint8_t new_header = 0;

    // Shift 4 bits to the left
    type = type << 4;

    // Bitwise 'or' operation to append
    // the response type into the start of header
    new_header = new_header | type;

    // Set compression related bits in the new_header
    if(compressed){
        // Set the compress bit to 1
        uint8_t bit = 1 << 3;
        new_header = new_header | bit;
    }

    // At this point we should have the new header craeted

    // Replace the old header with the new header
    packet->header = new_header;


}

/* 
Create a response using the information from the packet
And write it into the socket_fd
*/
void packet_to_response(struct data_packet* packet){

    // The length of the response should be 9 bytes + the payload length
    uint64_t length = htobe64(packet->payload_len);

    send(packet->socket_fd, &packet->header, 1, 0);

    send(packet->socket_fd, &length, 8, 0);

    // The socket buffer may not fit the entire payload
    int payload_sent = 0;
    while(payload_sent < packet->payload_len){
        int result = send(packet->socket_fd, &packet->payload[payload_sent], packet->payload_len-payload_sent, 0);
        if(result == -1){
            continue;
        }
        payload_sent += result;
    }


}

/*
Given a filename, append it to the back of the payload in packet
*/
void add_to_payload(struct data_packet* packet, char* filename){

    if(filename == NULL){
        
        uint8_t null_byte = 0x0;

        // Append a single null byte
        packet->payload = (uint8_t*)realloc(packet->payload, sizeof(uint8_t));
        memcpy(packet->payload, &null_byte, sizeof(uint8_t));

        // Update length
        packet->payload_len = 1;


        // Return 
        return;
    }


    uint64_t item_len = strlen(filename) + 1;

    // Check if the new filename will fit in the payload allocation
    if(packet->payload_len + item_len >= packet->payload_cap){
        // Reallocation is required 
        uint64_t new_capacity = (packet->payload_len + item_len) * 2;
        packet->payload = (uint8_t*)realloc(packet->payload, new_capacity);

        packet->payload_cap = new_capacity;

    }

    // memcpy each byte of the filename into the payload array
    memcpy(&packet->payload[packet->payload_len], filename, sizeof(uint8_t) * (item_len - 1));

    // Set the byte immediate after to be 0x0
    packet->payload[packet->payload_len + item_len - 1] = 0x0;
    
    // Update the index 
    packet->payload_len += item_len;

    return;

}

// Free payload
void cleanup_payload(struct data_packet* packet){

    free(packet->payload);

    return;
}

// Shutdown request
void terminate(struct data_packet* packet){

    // Cleanup payloads
    shutdown(packet->socket_fd, 0);
    close(packet->socket_fd);
    free(packet->payload);

    // Clean up all sessions
    struct session* cursor = *packet->sessions;
    while(cursor != NULL){
        struct session* clean = cursor;
        free(cursor->filename);
        cursor = cursor->next;
        free(clean);
    }

    // Clean up all connection allocations
    struct data_packet** packets = *packet->packets;
    size_t num_packets = *packet->num_packets;
    // Free all packets 
    for(size_t i = 0; i < num_packets; i++){
        if(packets[i]->socket_fd != 0){
            free(packets[i]);
        }

    }

    exit(0);
}

/*
Send back error response
*/
void error(struct data_packet* packet){
    // puts("called error");
    set_type(packet, 0xf, 0);

    packet->payload_len = 0;

    packet_to_response(packet);

    cleanup_payload(packet);

    return;

}

// Compress the payload in packet
void compress_payload(struct data_packet* packet){

    // New_payload capacity
    size_t payload_cap = packet->payload_len + 8;

    // New payload
    uint8_t* new_payload = (uint8_t*)calloc(payload_cap, sizeof(uint8_t));

    // bit index of where to continue writing
    size_t write_idx = 0;

    // For each byte in the payload
    for(size_t i = 0; i < packet->payload_len; i++){
        
        // Grab the byte
        uint8_t byte = packet->payload[i];
        // Find the corresponding compression encoding
        uint32_t encoding = packet->compress_arr[byte].encoding;

        uint32_t encoding_len = packet->compress_arr[byte].length;

        // The new byte length after the encoding is added
        size_t new_byte_length = ceil((double)(encoding_len + write_idx) / 8);

        // If adding the new encoding is going to exceed the available space of the payload
        if(new_byte_length >= payload_cap - 1 - 8){
            // Allocate more space
            // Allocate more space using calloc because we need memory zeroed out
            uint8_t* larger_payload = (uint8_t*)calloc(payload_cap*2, sizeof(uint8_t));
            // Copy across all the payload we have so far
            memcpy(larger_payload, new_payload, sizeof(uint8_t)*payload_cap);
            // Update capacity
            payload_cap *= 2;

            // free previous allocation
            free(new_payload);

            // Replace with new allocation
            new_payload = larger_payload;

        }

        // write into new payload
        bit_write(new_payload, write_idx, encoding, encoding_len);

        write_idx += encoding_len;

    }

    // Append a single byte which denotes how many zero paddings were at the end
    uint64_t new_payload_len = ceil((double)write_idx / 8);

    uint8_t padding = 8 - (write_idx % 8);
    if(padding == 8){
        new_payload[new_payload_len] = 0;
    } else {
        new_payload[new_payload_len] = padding;
    }
    new_payload_len++;

    // Number of bytes in the new payload
    packet->payload_len = new_payload_len;
    
    // Free the old payload
    free(packet->payload);

    // Store the new payload into the packet
    packet->payload = new_payload;

}

void decompress_payload(struct data_packet* packet){

    // Write index for the payload
    size_t write_idx = 0;

    // New decompressed payload
    uint8_t* new_payload = (uint8_t*)calloc(packet->payload_len, sizeof(uint8_t));

    size_t payload_cap = packet->payload_len;

    // num bits of padding at the end of compressed payload
    // should be given by the last byte of payload.
    size_t zero_paddings = packet->payload[packet->payload_len-1];

    // Number of bits to decode
    size_t num_bits = (packet->payload_len - 1) * 8 - zero_paddings;

    struct node* cursor = packet->decompress_tree;

    for(size_t i = 0; i < num_bits; i++){


        // Number of bits to shift is given by
        uint8_t bit_shift = 7 - (i % 8);

        uint8_t bit = bit_test(bit_shift, packet->payload[i / 8]);

        // Use the bit to navigate down the tree

        cursor = cursor->children[bit];

        // if this cursor has no more children, that means it's a leaf node
        if(cursor->children == NULL){
            // This node has the byte that we want to print out
            // We write it into the new_payload
            if(write_idx >= payload_cap){
                new_payload = (uint8_t*)realloc(new_payload, payload_cap*2);
                payload_cap *= 2;
            }

            new_payload[write_idx] = (uint8_t)cursor->symbol;
            write_idx++;
            
            // Update the cursor back to the root
            cursor = packet->decompress_tree;

        }


    }

    //  Replace old payload with new payload
    free(packet->payload);

    packet->payload = new_payload;

    // Update payload length
    packet->payload_len = write_idx;


}

/*
Echoe function
*/
void echo(struct data_packet* packet){

    // Change the response type to 0x1
    if(packet->compress){
        // If the payload was already compressed we return it the way it is
        set_type(packet, 0x1, 1);
    } else if (packet->req_compress){
        // The payload requires compression
        compress_payload(packet);
        set_type(packet, 0x1, 1);
    } else {
        set_type(packet, 0x1, 0);
    }
    
    // Return the same payload we were given
    packet_to_response(packet);
    
    cleanup_payload(packet);

    return;

}

void list_directory(struct data_packet* packet){


    // Read all the files in the target directory. 

    /* 
    For each file we have, we're going to store the name 
    into the packet payload, append a 0x0 byte.
    And record the payload length
    */

    // Initialise variabels that will be used in this function
    packet->payload_len = 0;
    packet->payload_cap = 0;


    DIR* d;

    struct dirent* file;

    d = opendir(packet->directory);

    // If the directory was opened successfully
    if(d){

        
        // For each file in the directory
        while((file = readdir(d)) != NULL){

            if(file->d_type == DT_REG){

            // store the filename 
            add_to_payload(packet, file->d_name);
            
            }


        }

        closedir(d);

    }

    if(packet->payload_len == 0){
        // No files were added
        add_to_payload(packet, NULL);

    }


   // After that, we can call packet_to_response because it should
   // Hold all the correct values in the packet

   if(packet->req_compress){
       // Payload requires compression
       compress_payload(packet);
       set_type(packet, 0x3, 1);
   } else {
       // Does not require compression
       set_type(packet, 0x3, 0);
   }

    packet_to_response(packet);

    cleanup_payload(packet);

}

void check_file_size(struct data_packet* packet){

    // Concatenate the directory and filename 
    char* filename = (char*)malloc(strlen(packet->directory) + strlen((char*)packet->payload) + 3);
    strcpy(filename, packet->directory);
    strcat(filename, "/");
    strcat(filename, (char*)packet->payload);

    FILE* file = fopen(filename, "r");

    if(file == NULL){
        error(packet);
        return;
    }

    fseek(file, 0L, SEEK_END);

    uint64_t file_size = ftell(file);

    file_size = htobe64(file_size);

    fclose(file);

    // Update payload length
    packet->payload_len = sizeof(uint64_t);

    // Memcpy the length into size into payload
    packet->payload = (uint8_t*)realloc(packet->payload, sizeof(uint64_t));

    memcpy(packet->payload, &file_size, sizeof(uint64_t));


    // Set header byte
    if(packet->req_compress){
        set_type(packet, 0x5, 1);
        compress_payload(packet);
    } else {
        set_type(packet, 0x5, 0);
    }

    packet_to_response(packet);

    free(filename);

    cleanup_payload(packet);

}

// Store content required for file retrieval
int init_file_content(struct data_packet* packet, struct session* session){

    // Convert both offset and length into host byte order
    uint64_t start_offset = be64toh(session->start_offset);
    uint64_t fetch_length = be64toh(session->length);

    // At index 20, the null terminated string should begin
    char* fname = (char*)&packet->payload[20];

    // Concatenate the directory and filename together
    char* filename = (char*)malloc(strlen(packet->directory) + strlen(fname) + 3);
    strcpy(filename, packet->directory);
    strcat(filename, "/");
    strcat(filename, fname);

    FILE* file = fopen(filename, "r");

    if(file == NULL){
        free(filename);
        error(packet);
        return 0;
    }

    // Navigate to end of file
    fseek(file, 0L, SEEK_END);
    // Record the file_size
    uint64_t file_size = ftell(file);
    // Rewind cursor to start retrieval
    fseek(file, start_offset, SEEK_SET);

    // printf("File size %lu\n", file_size);

    // request exceeds file size
    if(start_offset + fetch_length > file_size){
        free(filename);
        error(packet);
        return 0;
    }

    // Malloc the session payload, and put the entire retrieval in it.
    // session->payload = (uint8_t*)malloc(sizeof(uint8_t)*(fetch_length));
    
    // Read the entire file into the session->payload
    // fread(session->payload, sizeof(uint8_t), fetch_length, file);

    // Store the filename into sessino to be accessed by other threads.
    session->filepath = filename;

    fclose(file);

    // free(filename);

    return 1;

}


// Remove session from linked list
void remove_session(struct data_packet* packet, struct session* session){

    if(session->next == NULL && session->prev == NULL){
        // Only session in the linked list
        *packet->sessions = NULL;
        sem_destroy(&session->seg_lock);
        sem_destroy(&session->sent_lock);
        free(session->filename);
        free(session);
        return;
    }

    if(session->prev == NULL){
        // First session in the linked list
        // Let the next session become the head of the linkedlist
        *packet->sessions = session->next;
        session->next->prev = NULL;
        sem_destroy(&session->seg_lock);
        sem_destroy(&session->sent_lock);
        free(session->filename);
        free(session);
        return; 
    }

    if(session->next == NULL){
        // Last session in the linked list
        session->prev->next = NULL;
        sem_destroy(&session->seg_lock);
        sem_destroy(&session->sent_lock);
        free(session->filename);
        free(session);
        return;
    }

    // At this point the session belongs in the middle of the linkedlist
    session->prev->next = session->next;
    session->next->prev = session->prev;
    sem_destroy(&session->seg_lock);
    sem_destroy(&session->sent_lock);
    free(session->filename);
    free(session);
    return;




}

/* 
Create a new session using the sessionID and append it to the list of sessions
*/
struct session* create_new_session(struct data_packet* packet, uint32_t sessionID, uint64_t os, uint64_t len, char* filename){
    // If no sessions are ongoing
    if(*packet->sessions == NULL){
        // No sessions have been created so far
        struct session* new_session = (struct session*)malloc(sizeof(struct session));

        new_session->sessionID = sessionID;
        new_session->length = len;
        new_session->start_offset = os;
        new_session->filename = strdup(filename);

        int success = init_file_content(packet, new_session);

        if(!success){
            // File nonexistent, or bad range
            free(new_session);
            return NULL;
        }

        new_session->prev = NULL;
        new_session->next = NULL;
        new_session->handled_segments = 0;
        new_session->sent_segments = 0;
        sem_init(&new_session->seg_lock, 0, 1);
        sem_init(&new_session->sent_lock, 0, 1);
        *packet->sessions = new_session;

        new_session->completed = 0;

        return new_session;

    } else {

        // Ongoing sessions exist
        // Create new session and append it to end of list
        struct session* cursor = *packet->sessions;
        while(cursor->next != NULL){
            cursor = cursor->next;
        }
        // Cursor now points at the last node in the list

        struct session* new_session = (struct session*)malloc(sizeof(struct session));

        new_session->sessionID = sessionID;
        new_session->length = len;
        new_session->start_offset = os;
        new_session->filename = strdup(filename);


        int success = init_file_content(packet, new_session);

        if(!success){
            // File nonexistent, or bad range
            free(new_session);
            return NULL;
        }

        new_session->prev = cursor;
        new_session->next = NULL;
        new_session->handled_segments = 0;
        new_session->sent_segments = 0;
        sem_init(&new_session->seg_lock, 0, 1);
        sem_init(&new_session->sent_lock, 0, 1);

        new_session->completed = 0;

        cursor->next = new_session;
        return new_session;


    }



}

/*
Retrieve a session given a session ID
Return NULL if no session was found
*/
struct session* get_session(struct data_packet* packet, uint32_t sessionID, uint64_t os, uint64_t len, char* filename){

    // Check through the linked list for the session ID
    struct session* cursor = *packet->sessions;

    if(cursor == NULL){
        // No segments created thus far
        return create_new_session(packet, sessionID, os, len, filename);
    }

    while(1){

        if(cursor->sessionID == sessionID){
            // Found session with same ID

            // Check if the name of the file are the same
            if(strcmp(filename, cursor->filename) != 0 || cursor->start_offset != os || cursor->length != len){
                // File name not the same

                // If the previous session with this ID is completed, then we create and return a new session
                if(cursor->completed){
                    remove_session(packet, cursor);
                    return create_new_session(packet, sessionID, os, len, filename);
                } else {
                    // Otherwise, its an error
                    error(packet);
                    return NULL;

                }

            }

            // It's the same request
            if(cursor->completed){
                // The request has been completed already
                // We send back an empty payload
                set_type(packet, 0x7, 0);
                packet->payload_len = 0;
                packet_to_response(packet);
                cleanup_payload(packet);
                return NULL;
            }

            // Otherwise, return cursor
            return cursor;

        }

        // If cursor points to the last item in the list, break
        if(cursor->next == NULL){
            return create_new_session(packet, sessionID, os, len, filename);;
        } else {
            // Else continue with next session node
            cursor = cursor->next;
        }

    }


}



/*
Given the packet, payload, offset, length, session
Send back a segment of the file
*/
int send_segment(struct data_packet* packet, struct session* session){

    // Offset from the start of the retrieval (Note: Not the same as offset from start of file)
    uint64_t offset;
    // Length of this segment retrieval
    uint64_t length;
    // The segment number
    size_t segment; 

    sem_wait(&session->seg_lock);
    segment = session->handled_segments;
    session->handled_segments++;
    sem_post(&session->seg_lock);

    if(segment >= NUM_THREADS){
        // Another thread has handled the final segment
        return -1;
    }

    // Length in host byte order
    uint64_t host_len = be64toh(session->length);

    // Calculate offset and length that this segment will be handling
    offset = segment * (host_len/ NUM_THREADS);
    if(segment == NUM_THREADS - 1){
        // This is the last segment of the file retrieval
        length = host_len / NUM_THREADS;
        // We also have to include the remainder bytes that don't divide evenly by num threads
        length += host_len % NUM_THREADS;
        // printf("Included modulo remainder\n");
    } else {
        // Divide the length by number of threads to get length of this retrieval
        length = host_len / NUM_THREADS;

    }


    // Update the retrieve header
    *((uint32_t*)&packet->payload[0]) = session->sessionID;
    *((uint64_t*)&packet->payload[4]) = htobe64(be64toh(session->start_offset) + offset);
    *((uint64_t*)&packet->payload[12]) = htobe64(length);

    packet->payload_len = length + RETRIEVE_HEADER;

    packet->payload = (uint8_t*)realloc(packet->payload, sizeof(uint8_t)*packet->payload_len);

    // Copy the segment into the 20th position of the retrieval payload
    // memcpy(&packet->payload[20], &session->payload[offset], length);
    int host_offset = be64toh(session->start_offset);
    // Open the file and navigate to offset
    FILE* file = fopen(session->filepath, "r");
    fseek(file, offset + host_offset, SEEK_SET);

    fread(&packet->payload[20], sizeof(uint8_t), length, file);
    // printf("Seeked to offset %ld\n", offset + host_offset);

    fclose(file);


    // Handle header byte
    if(packet->req_compress){
        set_type(packet, 0x7, 1);
        compress_payload(packet);
    } else {
        // Does not require compression
        set_type(packet, 0x7, 0);
    }

    packet_to_response(packet);

    sem_wait(&session->sent_lock);
    session->sent_segments++;
    int num_sent = session->sent_segments;
    sem_post(&session->sent_lock);

    if(num_sent >= NUM_THREADS){
        // This thread handled the last segment of the file transfer
        session->completed = 1;
        // Free the paylaod
        // free(session->payload);
        return 0;

    }

    return 1;



}

// Retrieve request
void retrieve_file(struct data_packet* packet){

    if(packet->compress){
        // We need to decompress payload before continuing
        decompress_payload(packet);
    }


    // First 4 bytes of payload should represent the session ID
    uint32_t sessionID = *((uint32_t*)&packet->payload[0]);
    // Next 8 bytes gives starting offset
    uint64_t start_offset = *((uint64_t*)&packet->payload[4]);
    // Next four bytes gives length to retrieve
    uint64_t fetch_length = *((uint64_t*)&packet->payload[12]);
    // Name of file to fetch from

    char* filename = (char*)&packet->payload[20];

    // Returns a pointer to a session struct
    sem_wait(packet->session_lock);
    struct session* ses_ptr = get_session(packet, sessionID, start_offset, fetch_length, filename);
    sem_post(packet->session_lock);

    // If the ses_ptr is null, it is an invalid retrieval request
    if(ses_ptr == NULL){
        // free(filename);
        return;
    }

    // So now at this point, regardless of whether it is a new session or an existing session
    // THe ses_ptr should point to a valid session struct

    // number of segments this thread has helped sent
    int helped = 0;

    // The task is divded depending on how many threads we have in the thread pool
    while(1){

        int result = send_segment(packet, ses_ptr);

        if(result == 0){
            // Just finished handling the last segment of the file transfer
            break;
        } else if (result == -1){
            // No more segments to send
            if(helped == 0){
                // This thread didn't send any segments
                // Send back empty package
                set_type(packet, 0x7, 0);
                packet->payload_len = 0;
                packet_to_response(packet);
            }
            break;
        } else if (result == 1){
            // Successful send
            helped++;
        }

    }

    cleanup_payload(packet);


}



/*
Checks out the data_packet and call corresponding functions depending 
on the message header
*/
void* handle_request(void* arg){


    struct data_packet* packet = (struct data_packet*)arg;

    // Check the request type using header
    packet->type = determ_type(packet->header);

    // Check if payload is compressed

    packet->compress = bit_test(3, packet->header);

    // Check if response should be compressed
    packet->req_compress = bit_test(2, packet->header);

    // Next figure out which function to call depending on the request type
    // And pass through the packet into the appropriate function


    if(packet->type == 0){
        // Echo function
        echo(packet);
    } else if (packet->type == 2){
        // Directory listing
        list_directory(packet);
    } else if (packet->type == 4){
        // File size query
        check_file_size(packet);
    } else if (packet->type == 6){
        // Retrieve file
        retrieve_file(packet);
    } else if (packet->type == 8) {
        // shutdown 
        terminate(packet);

    } else {
        // Error 
        error(packet);
    }


    return NULL;

}



void add_to_cmp_array(struct node* array, uint8_t symbol, uint8_t len, uint64_t encoding){


    array[symbol].symbol = symbol;
    array[symbol].length = len;
    array[symbol].encoding = encoding;
    array[symbol].children = NULL;
    array[symbol].parent = NULL;

}

void add_to_decmp_tree(struct node* root, uint8_t symbol, uint8_t len, uint64_t encoding){

    // Navigate from the root node

    // Add children according to the encoding
    uint8_t depth = 0;
    
    // Keeps count of how many bits of the encoding has been read
    size_t bit_counter = 0;

    // Cursor to navigate the tree
    struct node* cursor = root;


    while(depth < len){
        
        uint64_t buf = be64toh(encoding);
        // Extract 1 bit from the encoding
        int bit = bit_extract((char*)&buf, (64 - len) + bit_counter, 1);
        bit_counter++;


        // check if cursor node has children
        if(cursor->children == NULL){
            // Create the children node pointers
            // Allocate for two children node pointers
            cursor->children = (struct node**)malloc(sizeof(struct node*)*2);
            cursor->children[0] = (struct node*)malloc(sizeof(struct node));
            cursor->children[1] = (struct node*)malloc(sizeof(struct node));

            cursor->children[0]->parent = cursor;
            cursor->children[1]->parent = cursor;

            cursor->children[0]->children = NULL;
            cursor->children[1]->children = NULL;
        }

        // At this point, the children node pointers should exist

        // We want to navigate to the one described by the bit
        cursor = cursor->children[bit];

        // Go one level deeper in the tree

        depth++;


    }

    // Cursor should now be pointing at the node to store this segment
    cursor->symbol = symbol;
    cursor->encoding = encoding;
    cursor->length = len;



}





void dict_setup(struct node* comp_array, struct node* decomp_tree){

    // Read compression.dict into compress and decompress tree

    FILE* dict_file = fopen("compression.dict", "rb");
    if(dict_file == NULL){
        // perror("No configuration file");
        return;
    }

    char dict[MAX_BUF_LEN];  
    // Grab all the data into the dict array as uint8_t bytes;
    fgets(dict, MAX_BUF_LEN, dict_file);

    size_t bit_index = 0;

    // For each dict segment
    for(size_t i = 0; i < NUM_SEGS; i++){
        
        // Extract length
        uint8_t length = bit_extract(dict, bit_index, 8);

        bit_index += 8;

        // Extract encoding

        uint64_t encoding = bit_extract(dict, bit_index, length);

        bit_index += length;

        // Add to compress tree
        add_to_cmp_array(comp_array, i, length, encoding);

        // Check value of encoding

        // Add to decompress tree
        add_to_decmp_tree(decomp_tree, i, length, encoding);


    }


}

void set_nonblocking(int fd){

    int flags = fcntl(fd, F_GETFL, 0);
    if(flags == -1){
        return;
    }

    if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1){
        return;
    }
    

}



int main(int argc, char** argv){

    // Fetch command line arguments
    if(argc != 2){
        puts("Incorrect number of arguments");
        return -1;
    }

    // Open configuration file 
    FILE* config = fopen(argv[1], "rb");
    if(config == NULL){
        puts("Could not open configuration file");
        return -1;
    }


    // Compression.dict set up

    struct node* compress_arr = (struct node*)malloc(sizeof(struct node)*NUM_SEGS);
    struct node* decompress_tree = (struct node*)malloc(sizeof(struct node));
    decompress_tree->children = NULL;
    decompress_tree->parent = NULL;

    dict_setup(compress_arr, decompress_tree);


    // Configuration file is available
    // Read the first 4 bytes into address variable
    uint32_t addr;
    uint16_t port;
    char directory[MAX_BUF_LEN];
    fread(&addr, 4, 1, config);
    fread(&port, 2, 1, config);
    // The remaining bytes is the path to the target directory where the server will offer files
    fgets(directory, MAX_BUF_LEN, config);

    fclose(config);


    // Create the IP socket address to store all this information into
    struct sockaddr_in address;
    int option = 1;

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = addr;
    address.sin_port = port;


    // Create socket

    int serversocket_fd = -1;
    // int clientsocket_fd = -1;

    serversocket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(serversocket_fd < 0){
        // puts("Error initialising socket");
        exit(1);
    }

    setsockopt(serversocket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &option, sizeof(int));

    if(bind(serversocket_fd, (struct sockaddr*)&address, sizeof(struct sockaddr_in))) {
        // perror("Binding error");
        exit(1);
    }

    // Set the socket to nonblocking

    set_nonblocking(serversocket_fd);
    listen(serversocket_fd, 128);

    struct data_packet** packets = (struct data_packet**)malloc(sizeof(struct data_packet*));

    struct session* sessions = NULL;

    // Semaphore to lock the session linked list
    sem_t ses_lock;
    sem_init(&ses_lock, 0, 1);

    size_t num_packets = 0;
    size_t packets_cap = 1;

    // We can handle up to num_threads events at a time
    struct epoll_event events[NUM_THREADS];

    // Create an epoll socket
    int epfd = epoll_create1(0);
    // Have epoll check for reading, and edge-triggered
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.data.fd = serversocket_fd;
    event.events = EPOLLIN | EPOLLET;
    if(epoll_ctl(epfd, EPOLL_CTL_ADD, event.data.fd, &event)){
        // perror("epoll_ctl()");
        return 1;
    }

    while(1){

        // Check how many requests are ready to be read

        size_t num_events = epoll_wait(epfd, events, NUM_THREADS, -1);

        if(num_events == -1){
            // perror("epoll_wait");
            return 0;
        }

        for(int i = 0; i < num_events; i++){

            if(events[i].events & EPOLLERR || (events[i].events & EPOLLHUP || (!(events[i].events & EPOLLIN)))) {
                // Error with event
                printf("Error event\n");
                close(events[i].data.fd);
                continue;
            }

            // Check if the fd is serversocket
            if(events[i].data.fd == serversocket_fd){

                // Accept all available connections
                while(1){

                    // New connection to the server
                    uint32_t addrlen = sizeof(struct sockaddr_in);

                    // Store the fd the connection is on into the ev struct
                    int new_client = accept(serversocket_fd, (struct sockaddr*)&address, &addrlen);

                    if(new_client == -1){
                        if(errno == EAGAIN || errno == EWOULDBLOCK){
                            // No more new connections
                            // perror("EWOULDBLOCK");
                            break;
                        } else {
                            // perror("Accept");
                            return 1;
                        }
                    } else {
                        // Successful connection
                        set_nonblocking(new_client);
                        event.data.fd = new_client;
                        event.events = EPOLLIN | EPOLLET;
                        if(epoll_ctl(epfd, EPOLL_CTL_ADD, new_client, &event) == -1) {
                            // perror("epoll_ctl()");
                            return 1;
                        }
                    }    

                }

            } else {
                // Client socket connection
                // Perform all the read inside main thread
                // Read all the information from the file descriptor into the struct

                uint8_t header_buf;

                while(1){

                    // Using a while loop in case there are multiple requests 
                    // Waiting to be read from the same socket
                    ssize_t header = read(events[i].data.fd, &header_buf, 1);

                    if(header == -1){
                        if(errno == EAGAIN || errno == EWOULDBLOCK){
                            // printf("No more requests from this socket\n");
                            break;
                        } else {
                            // perror("read error");
                            break;
                        }
                    } else if (header == 0){
                        // Connection lost with this client
                        // printf("Lost connection with this client\n");
                        close(events[i].data.fd);
                        break;
                    } else {
                        // Successful read on the header, read the length and payload

                        if(num_packets == packets_cap){
                            packets = (struct data_packet**)realloc(packets, sizeof(struct data_packet*)*packets_cap*2);
                            packets_cap *= 2;
                        }

                        packets[num_packets] = (struct data_packet*)malloc(sizeof(struct data_packet));

                        struct data_packet* packet = packets[num_packets];
                        num_packets++;
                        
                        // Next 8 byte should be length

                        ssize_t len = read(events[i].data.fd, &packet->payload_len, 8);

                        if(len == -1){
                            if(errno == EAGAIN || errno == EWOULDBLOCK){
                                // printf("Len: No more requests from this socket\n");
                                break;
                            } else {
                                // perror("read error");
                                break;
                            }
                        } else if (len == 0){
                            // Connection lost with this client
                            // printf("Len: Lost connection with this client\n");
                            close(events[i].data.fd);
                            break;
                        }

                        // Convert it to host bytes order
                        packet->payload_len = be64toh(packet->payload_len);
                        // Next read length bytes of payload
                        packet->payload = (uint8_t*)malloc(sizeof(uint8_t)*packet->payload_len);
                        if(packet->payload_len != 0){

                            ssize_t payload = 0;
                            while(payload != packet->payload_len){
                                // Keep checking whether enough data is in the socket
                                payload = recv(events[i].data.fd, packet->payload, packet->payload_len, MSG_PEEK);
                            }
                            // Breaks out if there is enough payload
                            payload = recv(events[i].data.fd, packet->payload, packet->payload_len, 0);


                            if(payload == -1){
                                if(errno == EAGAIN || errno == EWOULDBLOCK){
                                    // printf("Payload: No more requests from this socket\n");
                                    break;
                                } else {
                                    // perror("read error");
                                    break;
                                }
                            } else if (payload == 0){
                                // Connection lost with this client
                                break;
                            }
                        }

                        packet->header = header_buf;
                        packet->socket_fd = events[i].data.fd;
                        packet->directory = directory;
                        packet->num_packets = &num_packets;
                        packet->packets = &packets;
                        packet->compress_arr = compress_arr;
                        packet->decompress_tree = decompress_tree;
                        packet->sessions = &sessions;
                        packet->session_lock = &ses_lock;
                        packet->epfd = epfd;

                        // Begin handling the request
                        pthread_t thread;

                        pthread_create(&thread, NULL, handle_request, packet);

                        pthread_detach(thread);

                    }





                }



            }


            
        }



    }

    close(serversocket_fd);
    return 0;

    


}