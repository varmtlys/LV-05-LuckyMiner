#include "esp_log.h"
// #include "addr_from_stdin.h"
#include "connect.h"
#include "lwip/dns.h"
#include "work_queue.h"
#include "bm1397.h"
#include "global_state.h"
#include <string.h>
#include "stratum_task.h"
#include "nvs_config.h"
#include <esp_sntp.h>
#include <time.h>

#define PORT CONFIG_STRATUM_PORT
#define STRATUM_URL CONFIG_STRATUM_URL

#define STRATUM_PW CONFIG_STRATUM_PW
#define STRATUM_DIFFICULTY CONFIG_STRATUM_DIFFICULTY

// Backoff between reconnect attempts. Keeps a pool that refuses us from spinning the loop.
#define RECONNECT_DELAY_SECONDS 10

// How long a pool may stay silent before the connection is treated as dead.
#define POOL_SILENCE_TIMEOUT_SECONDS 300

static const char *TAG = "stratum_task";
static ip_addr_t ip_Addr;
static bool bDNSFound = false;
static bool bDNSInvalid = false;

static StratumApiV1Message stratum_api_v1_message = {};

static SystemTaskModule SYSTEM_TASK_MODULE = {
    .stratum_difficulty = 8192};

void dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
    // lwIP passes NULL when the name cannot be resolved. Dereferencing that panics the chip,
    // so a mistyped pool address became an endless reboot with no way to correct it.
    if (ipaddr == NULL)
    {
        bDNSInvalid = true;
    }
    else
    {
        ip_Addr = *ipaddr;
    }
    bDNSFound = true;
}

// Resolves the pool address into ip_Addr. Returns false when the name does not exist.
static bool _resolve_stratum_host(const char *url)
{
    if (inet_pton(AF_INET, url, &ip_Addr) == 1)
    {
        return true;
    }

    bDNSFound = false;
    bDNSInvalid = false;
    IP_ADDR4(&ip_Addr, 0, 0, 0, 0);

    ESP_LOGI(TAG, "Resolving %s", url);
    err_t err = dns_gethostbyname(url, &ip_Addr, dns_found_cb, NULL);

    if (err == ERR_OK)
    {
        // answer came from the cache and the callback will never fire
        return true;
    }
    if (err != ERR_INPROGRESS)
    {
        return false;
    }

    while (!bDNSFound)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    return !bDNSInvalid;
}

void stratum_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    STRATUM_V1_initialize_buffer();
    char host_ip[20];
    int addr_family = 0;
    int ip_protocol = 0;

    char *stratum_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, STRATUM_URL);
    uint16_t port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, PORT);

    // stratum_url stays allocated for the lifetime of this task; resolution is retried per
    // attempt so a bad or temporarily unreachable name is survivable instead of fatal.
    while (1)
    {
        struct sockaddr_in dest_addr;

        if (!_resolve_stratum_host(stratum_url))
        {
            ESP_LOGE(TAG, "Cannot resolve '%s' - check the pool address", stratum_url);
            sleep(RECONNECT_DELAY_SECONDS);
            continue;
        }

        snprintf(host_ip, sizeof(host_ip), "%d.%d.%d.%d",
                 ip4_addr1(&ip_Addr.u_addr.ip4),
                 ip4_addr2(&ip_Addr.u_addr.ip4),
                 ip4_addr3(&ip_Addr.u_addr.ip4),
                 ip4_addr4(&ip_Addr.u_addr.ip4));
        ESP_LOGI(TAG, "Connecting to: stratum+tcp://%s:%d (%s)", stratum_url, port, host_ip);

        dest_addr.sin_addr.s_addr = inet_addr(host_ip);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(port);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;

        // Retry rather than esp_restart(): an unreachable or unfriendly pool used to reboot the
        // device on a timer, which looks like a boot loop and loses the web interface with it.
        GLOBAL_STATE->sock = socket(addr_family, SOCK_STREAM, ip_protocol);
        if (GLOBAL_STATE->sock < 0)
        {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            sleep(RECONNECT_DELAY_SECONDS);
            continue;
        }
        ESP_LOGI(TAG, "Socket created, connecting to %s:%d", host_ip, port);

        // Without a receive timeout a half-open connection - a pool that vanished, a NAT that
        // dropped the mapping - parks this task in recv() forever: the miner looks alive and
        // hashes nothing. A pool sends work far more often than this, so silence means dead.
        struct timeval rx_timeout = {.tv_sec = POOL_SILENCE_TIMEOUT_SECONDS, .tv_usec = 0};
        setsockopt(GLOBAL_STATE->sock, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout, sizeof(rx_timeout));

        // Keepalive so the NAT mapping survives the long gaps between shares.
        int keepalive = 1, keepidle = 60, keepintvl = 15, keepcnt = 4;
        setsockopt(GLOBAL_STATE->sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(GLOBAL_STATE->sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
        setsockopt(GLOBAL_STATE->sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
        setsockopt(GLOBAL_STATE->sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

        int err = connect(GLOBAL_STATE->sock, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in6));
        if (err != 0)
        {
            ESP_LOGE(TAG, "Socket unable to connect: errno %d", errno);
            close(GLOBAL_STATE->sock);
            sleep(RECONNECT_DELAY_SECONDS);
            continue;
        }

        // Start with version rolling switched off. If the pool never grants a mask, the exact
        // version handed to us must be hashed, otherwise every share is rejected. BM1397 rolls
        // in software via midstates, which construct_bm_job skips while the mask is zero.
        GLOBAL_STATE->version_mask = 0;

        // BIP310 wants mining.configure before mining.subscribe. Pools that ignore it simply
        // never answer, and STRATUM_V1_subscribe just reads on until the subscribe result lands.
        STRATUM_V1_configure_version_rolling(GLOBAL_STATE->sock);

        if (STRATUM_V1_subscribe(GLOBAL_STATE->sock, GLOBAL_STATE->extranonce_str, EXTRANONCE_STR_SIZE,
                                 &GLOBAL_STATE->extranonce_2_len, ASIC_MODEL,
                                 &GLOBAL_STATE->version_mask) < 0)
        {
            ESP_LOGE(TAG, "Subscribe failed, reconnecting");
            shutdown(GLOBAL_STATE->sock, 0);
            close(GLOBAL_STATE->sock);
            sleep(RECONNECT_DELAY_SECONDS);
            continue;
        }

        char *username = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, STRATUM_USER);
        char *password = nvs_config_get_string(NVS_CONFIG_STRATUM_PASS, STRATUM_PW);
        STRATUM_V1_authenticate(GLOBAL_STATE->sock, username, password);
        free(username);
        free(password);

        ESP_LOGI(TAG, "Extranonce: %s", GLOBAL_STATE->extranonce_str);
        ESP_LOGI(TAG, "Extranonce 2 length: %d", GLOBAL_STATE->extranonce_2_len);

        STRATUM_V1_suggest_difficulty(GLOBAL_STATE->sock, STRATUM_DIFFICULTY);

        while (1)
        {
            char *line = STRATUM_V1_receive_jsonrpc_line(GLOBAL_STATE->sock);
            if (line == NULL)
            {
                ESP_LOGE(TAG, "Pool closed the connection, reconnecting");
                break;
            }

            ESP_LOGI(TAG, "rx: %s", line); // debug incoming stratum messages
            STRATUM_V1_parse(&stratum_api_v1_message, line);
            free(line);

            if (stratum_api_v1_message.method == MINING_NOTIFY)
            {
                SYSTEM_notify_new_ntime(&GLOBAL_STATE->SYSTEM_MODULE, stratum_api_v1_message.mining_notification->ntime);
                if (stratum_api_v1_message.should_abandon_work && (GLOBAL_STATE->stratum_queue.count > 0 || GLOBAL_STATE->ASIC_jobs_queue.count > 0))
                {
                    ESP_LOGI(TAG, "abandoning work");

                    GLOBAL_STATE->abandon_work = 1;
                    queue_clear(&GLOBAL_STATE->stratum_queue);

                    pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
                    ASIC_jobs_queue_clear(&GLOBAL_STATE->ASIC_jobs_queue);
                    for (int i = 0; i < 128; i = i + 4)
                    {
                        GLOBAL_STATE->valid_jobs[i] = 0;
                    }
                    pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);
                }
                if (GLOBAL_STATE->stratum_queue.count == QUEUE_SIZE)
                {
                    mining_notify *next_notify_json_str = (mining_notify *)queue_dequeue(&GLOBAL_STATE->stratum_queue);
                    STRATUM_V1_free_mining_notify(next_notify_json_str);
                }

                stratum_api_v1_message.mining_notification->difficulty = SYSTEM_TASK_MODULE.stratum_difficulty;
                queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification);
            }
            else if (stratum_api_v1_message.method == MINING_SET_DIFFICULTY)
            {
                if (stratum_api_v1_message.new_difficulty != SYSTEM_TASK_MODULE.stratum_difficulty)
                {
                    SYSTEM_TASK_MODULE.stratum_difficulty = stratum_api_v1_message.new_difficulty;
                    ESP_LOGI(TAG, "Set stratum difficulty: %g", SYSTEM_TASK_MODULE.stratum_difficulty);
                }
            }
            else if (stratum_api_v1_message.method == MINING_SET_VERSION_MASK || stratum_api_v1_message.method == STRATUM_RESULT_VERSION_MASK)
            {
                // 1fffe000
                ESP_LOGI(TAG, "Set version mask: %08lx", stratum_api_v1_message.version_mask);
                GLOBAL_STATE->version_mask = stratum_api_v1_message.version_mask;
            }
            else if (stratum_api_v1_message.method == MINING_SET_EXTRANONCE)
            {
                // Without this the pool rotates extranonce1 and every later share is stale.
                ESP_LOGI(TAG, "Set extranonce: %s (extranonce2 length %d)", stratum_api_v1_message.extranonce_str,
                         stratum_api_v1_message.extranonce_2_len);

                if (strlen(stratum_api_v1_message.extranonce_str) < EXTRANONCE_STR_SIZE)
                {
                    // overwrite in place - create_jobs_task reads this buffer from another task
                    strcpy(GLOBAL_STATE->extranonce_str, stratum_api_v1_message.extranonce_str);
                    GLOBAL_STATE->extranonce_2_len = stratum_api_v1_message.extranonce_2_len;
                }
                else
                {
                    ESP_LOGE(TAG, "Extranonce too long for the %d byte buffer, ignoring", EXTRANONCE_STR_SIZE);
                }

                free(stratum_api_v1_message.extranonce_str);
                stratum_api_v1_message.extranonce_str = NULL;
            }
            else if (stratum_api_v1_message.method == STRATUM_RESULT)
            {
                // Every boolean reply looks alike, so authorize and suggest_difficulty used to be
                // counted as accepted shares. Only count replies we actually have a share waiting for.
                SystemModule *sys = &GLOBAL_STATE->SYSTEM_MODULE;
                if ((uint16_t) (sys->shares_accepted + sys->shares_rejected) >= sys->shares_submitted)
                {
                    ESP_LOGI(TAG, "Pool replied %s (not a share)",
                             stratum_api_v1_message.response_success ? "ok" : "error");
                }
                else if (stratum_api_v1_message.response_success)
                {
                    ESP_LOGI(TAG, "message result accepted");
                    SYSTEM_notify_accepted_share(&GLOBAL_STATE->SYSTEM_MODULE);
                }
                else
                {
                    ESP_LOGE(TAG, "share rejected: %s", stratum_api_v1_message.error_str);
                    strncpy(sys->last_pool_error, stratum_api_v1_message.error_str,
                            sizeof(sys->last_pool_error) - 1);
                    sys->last_pool_error[sizeof(sys->last_pool_error) - 1] = 0;
                    SYSTEM_notify_rejected_share(&GLOBAL_STATE->SYSTEM_MODULE);
                }
            }
        }

        if (GLOBAL_STATE->sock != -1)
        {
            ESP_LOGE(TAG, "Shutting down socket and reconnecting...");
            shutdown(GLOBAL_STATE->sock, 0);
            close(GLOBAL_STATE->sock);
        }

        // back off so a pool that hangs up immediately cannot spin this loop
        sleep(RECONNECT_DELAY_SECONDS);
    }
    vTaskDelete(NULL);
}
