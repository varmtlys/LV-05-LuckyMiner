/******************************************************************************
 *  *
 * References:
 *  1. Stratum Protocol - [link](https://reference.cash/mining/stratum-protocol)
 *****************************************************************************/

#include "stratum_api.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "utils.h"

#define BUFFER_SIZE 1024
// How many lines the handshake will read while waiting for the subscribe result.
#define HANDSHAKE_MAX_LINES 10
static const char *TAG = "stratum_api";

static char *json_rpc_buffer = NULL;
static size_t json_rpc_buffer_size = 0;

// A message ID that must be unique per request that expects a response.
// For requests not expecting a response (called notifications), this is null.
static int send_uid = 1;

static void debug_stratum_tx(const char *);

void STRATUM_V1_initialize_buffer()
{
    json_rpc_buffer = malloc(BUFFER_SIZE);
    json_rpc_buffer_size = BUFFER_SIZE;
    memset(json_rpc_buffer, 0, BUFFER_SIZE);
    if (json_rpc_buffer == NULL)
    {
        printf("Error: Failed to allocate memory for buffer\n");
        exit(1);
    }
}

void cleanup_stratum_buffer()
{
    free(json_rpc_buffer);
}

static void realloc_json_buffer(size_t len)
{
    size_t old, new;

    old = strlen(json_rpc_buffer);
    new = old + len + 1;

    if (new < json_rpc_buffer_size)
    {
        return;
    }

    new = new + (BUFFER_SIZE - (new % BUFFER_SIZE));
    void *new_sockbuf = realloc(json_rpc_buffer, new);

    if (new_sockbuf == NULL)
    {
        fprintf(stderr, "Error: realloc failed in recalloc_sock()\n");
        esp_restart();
    }

    json_rpc_buffer = new_sockbuf;
    memset(json_rpc_buffer + old, 0, new - old);
    json_rpc_buffer_size = new;
}

char *STRATUM_V1_receive_jsonrpc_line(int sockfd)
{
    if (json_rpc_buffer == NULL)
    {
        STRATUM_V1_initialize_buffer();
    }
    char *line, *tok = NULL;
    char recv_buffer[BUFFER_SIZE];
    int nbytes;
    size_t buflen = 0;

    if (!strstr(json_rpc_buffer, "\n"))
    {
        do
        {
            memset(recv_buffer, 0, BUFFER_SIZE);
            nbytes = recv(sockfd, recv_buffer, BUFFER_SIZE - 1, 0);
            if (nbytes <= 0)
            {
                // -1: socket error, 0: the pool closed the connection.
                // Hand the failure back so the caller can reconnect. Restarting the chip here
                // turned a pool that hangs up during the handshake into an endless reboot loop,
                // and spinning on nbytes == 0 (the behaviour before that) hung the miner instead.
                ESP_LOGE(TAG, "recv returned %d, connection lost", nbytes);
                // drop the partial line so the next connection starts clean
                strcpy(json_rpc_buffer, "");
                return NULL;
            }

            realloc_json_buffer(nbytes);
            strncat(json_rpc_buffer, recv_buffer, nbytes);
        } while (!strstr(json_rpc_buffer, "\n"));
    }
    buflen = strlen(json_rpc_buffer);
    tok = strtok(json_rpc_buffer, "\n");
    line = strdup(tok);
    int len = strlen(line);
    if (buflen > len + 1)
        memmove(json_rpc_buffer, json_rpc_buffer + len + 1, buflen - len + 1);
    else
        strcpy(json_rpc_buffer, "");
    return line;
}

// cJSON_GetArrayItem returns NULL past the end of an array, and the params a pool sends are
// untrusted input. Without this every short or differently typed params array dereferenced
// NULL and rebooted the miner, which on a pool that keeps resending it is a reboot loop.
static const char *_array_string(cJSON *array, int index)
{
    cJSON *item = cJSON_GetArrayItem(array, index);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

void STRATUM_V1_parse(StratumApiV1Message *message, const char *stratum_json)
{
    // the caller reuses one message struct, so clear the owned pointer from the previous line
    message->extranonce_str = NULL;

    cJSON *json = cJSON_Parse(stratum_json);

    cJSON *id_json = cJSON_GetObjectItem(json, "id");
    int16_t parsed_id = -1;
    if (id_json != NULL && cJSON_IsNumber(id_json))
    {
        parsed_id = id_json->valueint;
    }
    message->message_id = parsed_id;

    cJSON *method_json = cJSON_GetObjectItem(json, "method");
    stratum_method result = STRATUM_UNKNOWN;
    if (method_json != NULL && cJSON_IsString(method_json))
    {
        if (strcmp("mining.notify", method_json->valuestring) == 0)
        {
            result = MINING_NOTIFY;
        }
        else if (strcmp("mining.set_difficulty", method_json->valuestring) == 0)
        {
            result = MINING_SET_DIFFICULTY;
        }
        else if (strcmp("mining.set_version_mask", method_json->valuestring) == 0)
        {
            result = MINING_SET_VERSION_MASK;
        }
        else if (strcmp("mining.set_extranonce", method_json->valuestring) == 0)
        {
            result = MINING_SET_EXTRANONCE;
        }
    }
    else
    {
        // parse results
        cJSON *result_json = cJSON_GetObjectItem(json, "result");
        cJSON *error_json = cJSON_GetObjectItem(json, "error");
        bool has_error = error_json != NULL && !cJSON_IsNull(error_json);

        // A rejected share comes back as {"result":null,"error":[21,"Job not found",null]}.
        // Testing only for a boolean result dropped those on the floor, so rejections went
        // uncounted and their reason was never seen.
        if ((result_json != NULL && cJSON_IsBool(result_json)) || has_error)
        {
            result = STRATUM_RESULT;
            message->response_success = cJSON_IsTrue(result_json) && !has_error;

            message->error_str[0] = 0;
            if (has_error)
            {
                cJSON *text = cJSON_IsArray(error_json) ? cJSON_GetArrayItem(error_json, 1) : error_json;
                if (cJSON_IsString(text))
                {
                    strncpy(message->error_str, text->valuestring, sizeof(message->error_str) - 1);
                    message->error_str[sizeof(message->error_str) - 1] = 0;
                }
            }
        }
        else
        {
            cJSON *mask = cJSON_GetObjectItem(result_json, "version-rolling.mask");
            if (mask != NULL)
            {
                result = STRATUM_RESULT_VERSION_MASK;
                message->version_mask = strtoul(mask->valuestring, NULL, 16);
            }
        }
    }

    message->method = result;

    if (message->method == MINING_NOTIFY)
    {

        cJSON *params = cJSON_GetObjectItem(json, "params");

        const char *job_id = _array_string(params, 0);
        const char *prev_block_hash = _array_string(params, 1);
        const char *coinbase_1 = _array_string(params, 2);
        const char *coinbase_2 = _array_string(params, 3);
        cJSON *merkle_branch = cJSON_GetArrayItem(params, 4);
        const char *version = _array_string(params, 5);
        const char *target = _array_string(params, 6);
        const char *ntime = _array_string(params, 7);

        int n_merkle_branches = cJSON_IsArray(merkle_branch) ? cJSON_GetArraySize(merkle_branch) : 0;

        bool usable = job_id != NULL && prev_block_hash != NULL && coinbase_1 != NULL && coinbase_2 != NULL &&
                      cJSON_IsArray(merkle_branch) && version != NULL && target != NULL && ntime != NULL;

        // Both hashes feed fixed 32 byte slots, and swap_endian_words() calls exit() on anything
        // not word aligned, so a pool sending an odd length here would take the miner down.
        if (usable && strlen(prev_block_hash) != HASH_SIZE * 2)
        {
            usable = false;
        }

        for (int i = 0; usable && i < n_merkle_branches; i++)
        {
            const char *branch = _array_string(merkle_branch, i);
            if (branch == NULL || strlen(branch) != HASH_SIZE * 2)
            {
                usable = false;
            }
        }

        if (!usable)
        {
            ESP_LOGE(TAG, "Malformed mining.notify, ignoring job: %s", stratum_json);
            message->method = STRATUM_UNKNOWN;
        }
        else if (n_merkle_branches > MAX_MERKLE_BRANCHES)
        {
            // used to abort(), which reboots the chip on a job it could simply skip
            ESP_LOGE(TAG, "Too many merkle branches (%d > %d), ignoring job", n_merkle_branches, MAX_MERKLE_BRANCHES);
            message->method = STRATUM_UNKNOWN;
        }
        else
        {
            mining_notify *new_work = malloc(sizeof(mining_notify));
            new_work->job_id = strdup(job_id);
            new_work->prev_block_hash = strdup(prev_block_hash);
            new_work->coinbase_1 = strdup(coinbase_1);
            new_work->coinbase_2 = strdup(coinbase_2);

            new_work->n_merkle_branches = n_merkle_branches;
            new_work->merkle_branches = malloc(HASH_SIZE * n_merkle_branches);
            for (int i = 0; i < n_merkle_branches; i++)
            {
                // HASH_SIZE, not HASH_SIZE * 2: the third argument is the size of the destination
                // slot, so the old value let a longer hex string write 64 bytes into 32.
                hex2bin(_array_string(merkle_branch, i), new_work->merkle_branches + HASH_SIZE * i, HASH_SIZE);
            }

            new_work->version = strtoul(version, NULL, 16);
            new_work->target = strtoul(target, NULL, 16);
            new_work->ntime = strtoul(ntime, NULL, 16);

            message->mining_notification = new_work;

            // params can be varible length
            int paramsLength = cJSON_GetArraySize(params);
            int value = cJSON_IsTrue(cJSON_GetArrayItem(params, paramsLength - 1));
            message->should_abandon_work = value;
        }
    }
    else if (message->method == MINING_SET_DIFFICULTY)
    {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        cJSON *difficulty = cJSON_GetArrayItem(params, 0);

        if (cJSON_IsNumber(difficulty))
        {
            // valuedouble, not valueint: pools with little hashrate hand out difficulties
            // below 1, and truncating those to 0 makes every nonce look like a valid share.
            message->new_difficulty = difficulty->valuedouble;
        }
        else
        {
            ESP_LOGE(TAG, "Malformed set_difficulty: %s", stratum_json);
            message->method = STRATUM_UNKNOWN;
        }
    }
    else if (message->method == MINING_SET_EXTRANONCE)
    {
        cJSON *params = cJSON_GetObjectItem(json, "params");
        cJSON *extranonce = cJSON_GetArrayItem(params, 0);
        cJSON *extranonce_2_len = cJSON_GetArrayItem(params, 1);

        if (cJSON_IsString(extranonce) && cJSON_IsNumber(extranonce_2_len))
        {
            message->extranonce_str = strdup(extranonce->valuestring);
            message->extranonce_2_len = extranonce_2_len->valueint;
        }
        else
        {
            ESP_LOGE(TAG, "Malformed set_extranonce: %s", stratum_json);
            message->method = STRATUM_UNKNOWN;
        }
    }
    else if (message->method == MINING_SET_VERSION_MASK)
    {

        cJSON *params = cJSON_GetObjectItem(json, "params");
        const char *mask = _array_string(params, 0);

        if (mask != NULL)
        {
            message->version_mask = strtoul(mask, NULL, 16);
        }
        else
        {
            ESP_LOGE(TAG, "Malformed set_version_mask: %s", stratum_json);
            message->method = STRATUM_UNKNOWN;
        }
    }

    cJSON_Delete(json);
}

void STRATUM_V1_free_mining_notify(mining_notify *params)
{
    free(params->job_id);
    free(params->prev_block_hash);
    free(params->coinbase_1);
    free(params->coinbase_2);
    free(params->merkle_branches);
    free(params);
}

// Returns 0 when the line really is a mining.subscribe result, -1 for anything else.
// Called on every line during the handshake, so a mismatch is normal and stays quiet.
int _parse_stratum_subscribe_result_message(const char *result_json_str,
                                            char *extranonce,
                                            size_t extranonce_size,
                                            int *extranonce2_len)
{
    int rc = -1;

    cJSON *root = cJSON_Parse(result_json_str);
    if (root == NULL)
    {
        ESP_LOGE(TAG, "Unable to parse %s", result_json_str);
        return -1;
    }

    // a subscribe result is [[subscriptions...], extranonce1, extranonce2_len]
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) >= 3)
    {
        cJSON *extranonce_json = cJSON_GetArrayItem(result, 1);
        cJSON *extranonce2_len_json = cJSON_GetArrayItem(result, 2);

        if (cJSON_IsString(extranonce_json) && cJSON_IsNumber(extranonce2_len_json) &&
            strlen(extranonce_json->valuestring) < extranonce_size)
        {
            *extranonce2_len = extranonce2_len_json->valueint;
            strcpy(extranonce, extranonce_json->valuestring);
            rc = 0;
        }
        else
        {
            ESP_LOGE(TAG, "Malformed subscribe result: %s", result_json_str);
        }
    }

    cJSON_Delete(root);

    return rc;
}

// Pulls the granted mask out of a mining.configure reply. Returns 0 when the line was one.
static int _parse_version_mask_result(const char *json_str, uint32_t *version_mask)
{
    int rc = -1;

    cJSON *root = cJSON_Parse(json_str);
    if (root == NULL)
    {
        return -1;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *mask = cJSON_GetObjectItem(result, "version-rolling.mask");
    if (cJSON_IsString(mask))
    {
        *version_mask = strtoul(mask->valuestring, NULL, 16);
        rc = 0;
    }

    cJSON_Delete(root);

    return rc;
}

int STRATUM_V1_subscribe(int socket, char * extranonce, size_t extranonce_size, int * extranonce2_len, char * model,
                         uint32_t * version_mask)
{
    // Subscribe
    char subscribe_msg[BUFFER_SIZE];
    sprintf(subscribe_msg, "{\"id\": %d, \"method\": \"mining.subscribe\", \"params\": [\"LuckyMiner %s\"]}\n", send_uid++, model);
    debug_stratum_tx(subscribe_msg);
    write(socket, subscribe_msg, strlen(subscribe_msg));

    // mining.configure went out first, so its reply can arrive before the subscribe result.
    // Read until the subscribe result turns up rather than assuming it is the very next line.
    for (int i = 0; i < HANDSHAKE_MAX_LINES; i++)
    {
        char * line = STRATUM_V1_receive_jsonrpc_line(socket);
        if (line == NULL)
        {
            ESP_LOGE(TAG, "Pool closed the connection during subscribe");
            return -1;
        }

        ESP_LOGI(TAG, "Received result %s", line);

        if (_parse_version_mask_result(line, version_mask) == 0)
        {
            ESP_LOGI(TAG, "Pool granted version mask: %08lx", *version_mask);
        }
        else if (_parse_stratum_subscribe_result_message(line, extranonce, extranonce_size, extranonce2_len) == 0)
        {
            free(line);
            return 1;
        }

        free(line);
    }

    ESP_LOGE(TAG, "No subscribe result from pool after %d lines", HANDSHAKE_MAX_LINES);

    return -1;
}

int STRATUM_V1_suggest_difficulty(int socket, uint32_t difficulty)
{
    char difficulty_msg[BUFFER_SIZE];
    sprintf(difficulty_msg, "{\"id\": %d, \"method\": \"mining.suggest_difficulty\", \"params\": [%ld]}\n", send_uid++, difficulty);
    debug_stratum_tx(difficulty_msg);
    write(socket, difficulty_msg, strlen(difficulty_msg));

    /* TODO: fix race condition with first mining.notify message
    char * line;
    line = STRATUM_V1_receive_jsonrpc_line(socket);

    ESP_LOGI(TAG, "Received result %s", line);

    free(line);
    */

    return 1;
}

int STRATUM_V1_authenticate(int socket, const char *username, const char *password)
{
    char authorize_msg[BUFFER_SIZE];
    // The password was hardcoded to "x". Multi-coin pools carry options in it, such as
    // zpool's "c=PPC" that selects the payout currency, so it has to be configurable.
    sprintf(authorize_msg, "{\"id\": %d, \"method\": \"mining.authorize\", \"params\": [\"%s\", \"%s\"]}\n",
            send_uid++, username, password);
    debug_stratum_tx(authorize_msg);

    write(socket, authorize_msg, strlen(authorize_msg));

    return 1;
}

/// @param socket Socket to write to
/// @param username The client’s user name.
/// @param jobid The job ID for the work being submitted.
/// @param ntime The hex-encoded time value use in the block header.
/// @param extranonce_2 The hex-encoded value of extra nonce 2.
/// @param nonce The hex-encoded nonce value to use in the block header.
void STRATUM_V1_submit_share(int socket, const char *username, const char *jobid,
                             const char *extranonce_2, const uint32_t ntime, const uint32_t nonce,
                             const uint32_t version, const bool send_version_bits)
{
    char submit_msg[BUFFER_SIZE];

    if (send_version_bits)
    {
        sprintf(submit_msg, "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\", \"%08lx\"]}\n",
                send_uid++, username, jobid, extranonce_2, ntime, nonce, version);
    }
    else
    {
        // A pool that never negotiated version rolling rejects the 6th parameter.
        sprintf(submit_msg, "{\"id\": %d, \"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%08lx\", \"%08lx\"]}\n",
                send_uid++, username, jobid, extranonce_2, ntime, nonce);
    }

    debug_stratum_tx(submit_msg);
    write(socket, submit_msg, strlen(submit_msg));
}

void STRATUM_V1_configure_version_rolling(int socket)
{
    // Configure
    char configure_msg[BUFFER_SIZE * 2];
    sprintf(configure_msg, "{\"id\": %d, \"method\": \"mining.configure\", \"params\": [[\"version-rolling\"], {\"version-rolling.mask\": \"ffffffff\"}]}\n", send_uid++);
    ESP_LOGI(TAG, "tx: %s", configure_msg);
    write(socket, configure_msg, strlen(configure_msg));

    return;
}

static void debug_stratum_tx(const char *msg)
{
    ESP_LOGI(TAG, "tx: %s", msg);
}
