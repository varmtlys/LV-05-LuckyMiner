#ifndef GLOBAL_STATE_H_
#define GLOBAL_STATE_H_

#include "asic_task.h"
#include "bm1397.h"
#include "common.h"
#include "power_management_task.h"
#include "serial.h"
#include "stratum_api.h"
#include "system.h"
#include "work_queue.h"

#define STRATUM_USER CONFIG_STRATUM_USER

// extranonce1 as hex. Pools use 8-16 bytes, so 64 chars leaves plenty of headroom.
#define EXTRANONCE_STR_SIZE 64

// The only ASIC this board carries.
#define ASIC_MODEL "BM1397"

typedef struct
{
    double asic_job_frequency_ms;

    work_queue stratum_queue;
    work_queue ASIC_jobs_queue;

    bm1397Module BM1397_MODULE;
    SystemModule SYSTEM_MODULE;
    AsicTaskModule ASIC_TASK_MODULE;
    PowerManagementModule POWER_MANAGEMENT_MODULE;

    // A fixed buffer, not a heap pointer: create_jobs_task reads this continuously from another
    // task, and mining.set_extranonce can replace it mid-run. Freeing it would dangle under that
    // reader; overwriting in place cannot.
    char extranonce_str[EXTRANONCE_STR_SIZE];
    int extranonce_2_len;
    int abandon_work;

    uint8_t * valid_jobs;
    pthread_mutex_t valid_jobs_lock;

    // what the chip is currently programmed to report at, and the power of two it rounded to
    double stratum_difficulty;
    double asic_ticket_diff;
    uint32_t version_mask;

    int sock;

} GlobalState;

#endif /* GLOBAL_STATE_H_ */