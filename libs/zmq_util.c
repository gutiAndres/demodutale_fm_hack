//libs/zmq_util.c
#include "zmq_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

zpub_t* zpub_init(void) {
    zpub_t *pub = malloc(sizeof(zpub_t));
    if (!pub) return NULL;

    pub->context = zmq_ctx_new();
    pub->socket = zmq_socket(pub->context, ZMQ_PUB);

    // BINDING: The Publisher creates the file. 
    // The Python Subscriber will CONNECT to this.
    int rc = zmq_bind(pub->socket, PUB_IPC_ADDR);
    
    if (rc != 0) {
        fprintf(stderr, "[ZPUB] Error: Could not bind to %s. (Is another process holding it?)\n", PUB_IPC_ADDR);
        free(pub);
        return NULL;
    }

    printf("[ZPUB] Publisher bound to %s\n", PUB_IPC_ADDR);
    return pub;
}

int zpub_publish(zpub_t *pub, const char *topic, const char *json_payload) {
    if (!pub || !topic || !json_payload) return -1;

    // 1. Calculate required length: Topic + Space + JSON + NullTerminator
    int len = snprintf(NULL, 0, "%s %s", topic, json_payload);
    if (len < 0) return -1;

    // 2. Allocate buffer
    char *buffer = malloc(len + 1);
    if (!buffer) return -1;

    // 3. Format the string: "topic {json}"
    snprintf(buffer, len + 1, "%s %s", topic, json_payload);

    // 4. Send
    // We send as a single string to match the Python split(" ", 1) logic
    int bytes_sent = zmq_send(pub->socket, buffer, len, 0);

    free(buffer);
    return bytes_sent;
}

void zpub_close(zpub_t *pub) {
    if (pub) {
        if (pub->socket) zmq_close(pub->socket);
        if (pub->context) zmq_ctx_term(pub->context);
        free(pub);
        printf("[ZPUB] Publisher closed.\n");
    }
}

static void* listener_thread(void *arg) {
    zsub_t *sub = (zsub_t*)arg;
    
    while (sub->running) {
        // Attempt to receive (blocks for max 1 second due to RCVTIMEO)
        int len = zmq_recv(sub->socket, sub->buffer, ZSUB_BUF_SIZE - 1, 0);
        
        if (len > 0) {
            sub->buffer[len] = '\0';
            char *json_payload = strchr(sub->buffer, ' ');
            if (json_payload && sub->callback) {
                sub->callback(json_payload + 1);
            }
        } else {
            // This else block runs when we time out. 
            // The thread is "awake" here but found no data.
            // It allows the while(sub->running) check to happen.
        }
    }
    return NULL;
}

zsub_t* zsub_init(const char *topic, msg_callback_t cb) {
    zsub_t *sub = malloc(sizeof(zsub_t));
    sub->context = zmq_ctx_new();
    sub->socket = zmq_socket(sub->context, ZMQ_SUB);
    sub->callback = cb;
    sub->running = 0;

    // FIX 1: Set a 1-second timeout so the thread wakes up to check 'running' flag
    int timeout = 1000; 
    zmq_setsockopt(sub->socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

    zmq_connect(sub->socket, IPC_ADDR);
    zmq_setsockopt(sub->socket, ZMQ_SUBSCRIBE, topic, strlen(topic));
    
    return sub;
}

void zsub_start(zsub_t *sub) {
    sub->running = 1;
    pthread_create(&sub->thread_id, NULL, listener_thread, sub);
    printf("[ZMQ] Background thread started.\n");
}

void zsub_close(zsub_t *sub) {
    sub->running = 0; // Signal thread to stop
    pthread_join(sub->thread_id, NULL); // Wait for current timeout cycle to finish
    zmq_close(sub->socket);
    zmq_ctx_term(sub->context);
    free(sub);
}