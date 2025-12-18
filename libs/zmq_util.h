//libs/zmq_util.h
#ifndef ZMQPUB_H
#define ZMQPUB_H

#include <zmq.h>
#include <pthread.h>

#define IPC_ADDR "ipc:///tmp/zmq_feed"
#define PUB_IPC_ADDR "ipc:///tmp/zmq_data"
#define ZSUB_BUF_SIZE 1024

// Define the type for the function you want to run when data arrives
typedef void (*msg_callback_t)(const char *payload);
typedef struct {
    void *context;
    void *socket;
    char buffer[ZSUB_BUF_SIZE];
    pthread_t thread_id;
    msg_callback_t callback;
    int running;
} zsub_t;
typedef struct {
    void *context;
    void *socket;
} zpub_t;

zsub_t* zsub_init(const char *topic, msg_callback_t cb);
void zsub_start(zsub_t *sub);
void zsub_close(zsub_t *sub);
zpub_t* zpub_init(void);
int zpub_publish(zpub_t *pub, const char *topic, const char *json_payload);
void zpub_close(zpub_t *pub);

#endif