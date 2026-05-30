#ifndef DAG_CONF_H
#define DAG_CONF_H

/* Ring Buffer constants for TaskID indexing */
#define RING_SIZE 4096
#define RING_MASK (RING_SIZE - 1)
#define HALF_RING_SIZE 2048
#define SUCC_NODE_CNT 3
#define THREAD_CNT 2

#define AIC_OSTD 2
#define AIC_CNT 60

#endif /* DAG_CONF_H */
