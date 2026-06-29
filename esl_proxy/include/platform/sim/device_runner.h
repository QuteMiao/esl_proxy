#ifndef ESL_PROXY_DEVICE_RUNNER_H
#define ESL_PROXY_DEVICE_RUNNER_H

typedef struct EslRuntime EslRuntime;

int esl_sim_aicore_workers_start(EslRuntime *runtime);
void esl_sim_aicore_workers_stop(void);

#endif /* ESL_PROXY_DEVICE_RUNNER_H */
