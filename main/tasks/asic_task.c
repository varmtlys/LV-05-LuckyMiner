#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "work_queue.h"
#include "serial.h"
#include "bm1397.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_config.h"

static const char *TAG = "ASIC_task";

// Cap on the chip's reporting threshold. A pool difficulty of 16384 makes the chip report once
// every few minutes, which is far too rare to measure a hashrate from; capping the ticket mask
// costs a fraction of a result per second and gives a usable estimate within a minute. Results
// below the pool difficulty are simply never submitted.
#define ASIC_TICKET_DIFF_CAP 256

void ASIC_task(void *pvParameters)
{

    GlobalState *GLOBAL_STATE = (GlobalState *)pvParameters;

    GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs = malloc(sizeof(bm_job *) * 128);
    GLOBAL_STATE->valid_jobs = malloc(sizeof(uint8_t) * 128);
    for (int i = 0; i < 128; i++)
    {

        GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[i] = NULL;
        GLOBAL_STATE->valid_jobs[i] = 0;
    }

    int baud = BM1397_set_max_baud();
    vTaskDelay(10 / portTICK_PERIOD_MS);
    SERIAL_set_baud(baud);

    SYSTEM_notify_mining_started(&GLOBAL_STATE->SYSTEM_MODULE);
    ESP_LOGI(TAG, "ASIC Ready!");
    while (1)
    {

        // a fault stopped mining; idle here so the rest of the system stays responsive
        if (GLOBAL_STATE->SYSTEM_MODULE.halt_reason != NULL) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        bm_job *next_bm_job = (bm_job *)queue_dequeue(&GLOBAL_STATE->ASIC_jobs_queue);

        // the ASIC ticket mask is a power of two, so a pool difficulty below 1 must not reach it as 0
        double ticket = next_bm_job->pool_diff < 1 ? 1 : next_bm_job->pool_diff;
        if (ticket > ASIC_TICKET_DIFF_CAP)
        {
            ticket = ASIC_TICKET_DIFF_CAP;
        }

        if (ticket != GLOBAL_STATE->stratum_difficulty)
        {
            BM1397_set_job_difficulty_mask((int) ticket);
            GLOBAL_STATE->stratum_difficulty = ticket;
            // what the chip actually ended up with, and the weight of every result it reports
            GLOBAL_STATE->asic_ticket_diff = _largest_power_of_two((int) ticket);
        }

        BM1397_send_work(GLOBAL_STATE, next_bm_job);

        // Follow the frequency the power management task actually settled on, not the one asked
        // for in NVS. While the chip ramps up or throttles down, a cadence fixed at the target
        // replaces each job before the chip has finished its nonce space.
        double frequency = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
        if (frequency < 1)
        {
            frequency = 1;
        }
        GLOBAL_STATE->asic_job_frequency_ms = ((double) NONCE_SPACE / (frequency * BM1397_CORE_COUNT * 1000000)) * 1000;

        // Time to execute the above code is ~0.3ms
        vTaskDelay((GLOBAL_STATE->asic_job_frequency_ms - 0.3) / portTICK_PERIOD_MS);
    }
}
