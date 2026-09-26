#include "global_state.h"
#include "work_queue.h"
#include "serial.h"
#include "bm1397.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_config.h"
#include "utils.h"

const char *TAG = "asic_result";

void ASIC_result_task(void *pvParameters)
{
    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;
    SERIAL_clear_buffer();

    char *user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, STRATUM_USER);

    while (1)
    {

        task_result *asic_result = BM1397_proccess_work(GLOBAL_STATE);

        if (asic_result == NULL)
        {
            continue;
        }

        GLOBAL_STATE->SYSTEM_MODULE.asic_results++;

        uint8_t job_id = asic_result->job_id;

        // Work on a copy. The job was abandoned (a new block, or clean_jobs) or its slot was
        // recycled by the sender the moment the lock is released, and the fields below are read
        // across a network send - a pointer into active_jobs cannot survive that.
        bm_job job;
        pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock);
        bool valid = GLOBAL_STATE->valid_jobs[job_id] != 0 && GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id] != NULL;
        if (valid)
        {
            job = *GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id];
        }
        pthread_mutex_unlock(&GLOBAL_STATE->valid_jobs_lock);

        if (!valid)
        {
            ESP_LOGD(TAG, "Stale nonce for job %d, dropping", job_id);
            continue;
        }

        double nonce_diff = test_nonce_value(&job, asic_result->nonce, asic_result->rolled_version);

        ESP_LOGD(TAG, "Nonce difficulty %.2f of %g.", nonce_diff, job.pool_diff);

        // Every result the chip reports is a hashrate sample, not just the ones good enough to
        // send. The ticket difficulty is the chip's own reporting threshold, so it is the right
        // weight - and it arrives orders of magnitude more often than a pool share.
        SYSTEM_notify_found_nonce(&GLOBAL_STATE->SYSTEM_MODULE, GLOBAL_STATE->asic_ticket_diff, nonce_diff, job.target);

        if (nonce_diff > job.pool_diff)
        {
            GLOBAL_STATE->SYSTEM_MODULE.shares_submitted++;

            STRATUM_V1_submit_share(
                GLOBAL_STATE->sock,
                user,
                job.jobid,
                job.extranonce2,
                job.ntime,
                asic_result->nonce,
                asic_result->rolled_version ^ job.version,
                GLOBAL_STATE->version_mask != 0);
        }
    }
}
