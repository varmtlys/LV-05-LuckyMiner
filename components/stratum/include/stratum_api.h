#ifndef STRATUM_API_H
#define STRATUM_API_H

#include "cJSON.h"
#include <stdint.h>
#include <stdbool.h>

#define MAX_MERKLE_BRANCHES 32
#define HASH_SIZE 32
#define COINBASE_SIZE 100
#define COINBASE2_SIZE 128

typedef enum
{
    STRATUM_UNKNOWN,
    MINING_NOTIFY,
    MINING_SET_DIFFICULTY,
    MINING_SET_VERSION_MASK,
    MINING_SET_EXTRANONCE,
    STRATUM_RESULT,
    STRATUM_RESULT_VERSION_MASK
} stratum_method;

typedef struct
{
    char *job_id;
    char *prev_block_hash;
    char *coinbase_1;
    char *coinbase_2;
    uint8_t *merkle_branches;
    size_t n_merkle_branches;
    uint32_t version;
    uint32_t version_mask;
    uint32_t target;
    uint32_t ntime;
    double difficulty;
} mining_notify;

typedef struct
{

    int16_t message_id;
    // Indicates the type of request the message represents.
    stratum_method method;

    // mining.notify
    int should_abandon_work;
    mining_notify *mining_notification;
    // mining.set_difficulty
    double new_difficulty;
    // mining.set_version_mask
    uint32_t version_mask;
    // mining.set_extranonce - caller takes ownership of extranonce_str and must free it
    char *extranonce_str;
    int extranonce_2_len;
    // result
    bool response_success;
    // reason text from a rejecting pool, empty when there was none
    char error_str[64];
} StratumApiV1Message;

void STRATUM_V1_initialize_buffer();

char *STRATUM_V1_receive_jsonrpc_line(int sockfd);

// Reads lines until the subscribe result arrives. If a mining.configure reply shows up first
// (it does when configure was sent before subscribe), its granted mask lands in version_mask.
// extranonce is a caller-owned buffer of extranonce_size bytes.
int STRATUM_V1_subscribe(int socket, char * extranonce, size_t extranonce_size, int * extranonce2_len, char * model,
                         uint32_t * version_mask);

void STRATUM_V1_parse(StratumApiV1Message *message, const char *stratum_json);

void STRATUM_V1_free_mining_notify(mining_notify *params);

int STRATUM_V1_authenticate(int socket, const char *username, const char *password);

void STRATUM_V1_configure_version_rolling(int socket);

int STRATUM_V1_suggest_difficulty(int socket, uint32_t difficulty);

// send_version_bits must only be true when the pool granted version rolling. Pools that did not
// negotiate it reject a 6-parameter submit outright.
void STRATUM_V1_submit_share(int socket, const char *username, const char *jobid,
                             const char *extranonce_2, const uint32_t ntime, const uint32_t nonce,
                             const uint32_t version, const bool send_version_bits);

#endif // STRATUM_API_H