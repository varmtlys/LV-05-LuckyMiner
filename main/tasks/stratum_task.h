#ifndef STRATUM_TASK_H_
#define STRATUM_TASK_H_

typedef struct
{
    // double, not uint32_t: pools may set a difficulty below 1
    double stratum_difficulty;
} SystemTaskModule;

void stratum_task(void *pvParameters);

#endif