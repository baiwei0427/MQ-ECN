#include <linux/sysctl.h>
#include <linux/string.h>

#include "params.h"

/* Debug mode or not. By default, we disable debug mode */
int WFQ_QDISC_DEBUG_MODE = WFQ_QDISC_DEBUG_OFF;
/* Buffer management mode: shared (0) or static (1). By default, we enable shread buffer. */
int WFQ_QDISC_BUFFER_MODE = WFQ_QDISC_SHARED_BUFFER;
/* Per port shared buffer (bytes) */
int WFQ_QDISC_SHARED_BUFFER_BYTES = WFQ_QDISC_MAX_BUFFER_BYTES;
/* Bucket size in nanosecond. By default, we use 25us for 1G network. */
int WFQ_QDISC_BUCKET_NS = 25000;
/* Per port ECN marking threshold (bytes). By default, we use 32KB for 1G network. */
int WFQ_QDISC_PORT_THRESH_BYTES = 32000;
/* ECN marking scheme. By default, we use per queue ECN. */
int WFQ_QDISC_ECN_SCHEME = WFQ_QDISC_QUEUE_ECN;

int WFQ_QDISC_DEBUG_MODE_MIN = WFQ_QDISC_DEBUG_OFF;
int WFQ_QDISC_DEBUG_MODE_MAX = WFQ_QDISC_DEBUG_ON;
int WFQ_QDISC_BUFFER_MODE_MIN = WFQ_QDISC_SHARED_BUFFER;
int WFQ_QDISC_BUFFER_MODE_MAX = WFQ_QDISC_STATIC_BUFFER;
int WFQ_QDISC_ECN_SCHEME_MIN = WFQ_QDISC_DISABLE_ECN;
int WFQ_QDISC_ECN_SCHEME_MAX = WFQ_QDISC_DEQUE_ECN;
int WFQ_QDISC_DSCP_MIN = 0;
int WFQ_QDISC_DSCP_MAX = 63;
int WFQ_QDISC_WEIGHT_MIN = 1;
int WFQ_QDISC_WEIGHT_MAX = WFQ_QDISC_MIN_PKT_BYTES;

/* Per queue ECN marking threshold (bytes) */
int WFQ_QDISC_QUEUE_THRESH_BYTES[WFQ_QDISC_MAX_QUEUES];
/* DSCP value for different queues */
int WFQ_QDISC_QUEUE_DSCP[WFQ_QDISC_MAX_QUEUES];
/* Weights for different queues*/
int WFQ_QDISC_QUEUE_WEIGHT[WFQ_QDISC_MAX_QUEUES];
/* Per queue static reserved buffer (bytes) */
int WFQ_QDISC_QUEUE_BUFFER_BYTES[WFQ_QDISC_MAX_QUEUES];

/* All parameters that can be configured through sysctl. We have 6 + 4*WFQ_QDISC_MAX_QUEUES parameters in total. */
struct WFQ_QDISC_Param WFQ_QDISC_Params[6 + 4 * WFQ_QDISC_MAX_QUEUES + 1] =
{
	{"debug_mode", &WFQ_QDISC_DEBUG_MODE},
	{"buffer_mode",&WFQ_QDISC_BUFFER_MODE},
	{"shared_buffer_bytes", &WFQ_QDISC_SHARED_BUFFER_BYTES},
	{"bucket_ns", &WFQ_QDISC_BUCKET_NS},
	{"port_thresh_bytes", &WFQ_QDISC_PORT_THRESH_BYTES},
	{"ecn_scheme",&WFQ_QDISC_ECN_SCHEME},
};

struct ctl_table WFQ_QDISC_Params_table[6 + 4 * WFQ_QDISC_MAX_QUEUES + 1];

struct ctl_path WFQ_QDISC_Params_path[] =
{
	{ .procname = "wfq" },
	{ },
};

struct ctl_table_header *WFQ_QDISC_Sysctl = NULL;

int wfq_qdisc_params_init()
{
    int i;
	memset(WFQ_QDISC_Params_table, 0, sizeof(WFQ_QDISC_Params_table));

    for (i = 0; i < WFQ_QDISC_MAX_QUEUES; i++)
    {
        /* Initialize WFQ_QDISC_QUEUE_THRESH_BYTES[WFQ_QDISC_MAX_QUEUES]*/
        snprintf(WFQ_QDISC_Params[6 + i].name, 63,"queue_thresh_bytes_%d", i);
        WFQ_QDISC_Params[6 + i].ptr = &WFQ_QDISC_QUEUE_THRESH_BYTES[i];
        WFQ_QDISC_QUEUE_THRESH_BYTES[i] = WFQ_QDISC_PORT_THRESH_BYTES;

        /* Initialize WFQ_QDISC_QUEUE_DSCP[WFQ_QDISC_MAX_QUEUES] */
        snprintf(WFQ_QDISC_Params[6 + i + WFQ_QDISC_MAX_QUEUES].name, 63, "queue_dscp_%d", i);
        WFQ_QDISC_Params[6 + i + WFQ_QDISC_MAX_QUEUES].ptr = &WFQ_QDISC_QUEUE_DSCP[i];
        WFQ_QDISC_QUEUE_DSCP[i] = i;

        /* Initialize WFQ_QDISC_QUEUE_WEIGHT[WFQ_QDISC_MAX_QUEUES] */
        snprintf(WFQ_QDISC_Params[6 + i + 2 * WFQ_QDISC_MAX_QUEUES].name, 63, "queue_weight_%d", i);
        WFQ_QDISC_Params[6 + i + 2 * WFQ_QDISC_MAX_QUEUES].ptr = &WFQ_QDISC_QUEUE_WEIGHT[i];
        WFQ_QDISC_QUEUE_WEIGHT[i] = 1;

        /* Initialize WFQ_QDISC_QUEUE_BUFFER_BYTES[WFQ_QDISC_MAX_QUEUES] */
        snprintf(WFQ_QDISC_Params[6 + i + 3 * WFQ_QDISC_MAX_QUEUES].name, 63, "queue_buffer_bytes_%d", i);
        WFQ_QDISC_Params[6 + i + 3 * WFQ_QDISC_MAX_QUEUES].ptr = &WFQ_QDISC_QUEUE_BUFFER_BYTES[i];
        WFQ_QDISC_QUEUE_BUFFER_BYTES[i] = WFQ_QDISC_MAX_BUFFER_BYTES;
    }

    /* End of the parameters */
    WFQ_QDISC_Params[6 + 4 * WFQ_QDISC_MAX_QUEUES].ptr = NULL;

    for (i = 0; i < 6 + 4 * WFQ_QDISC_MAX_QUEUES; i++)
    {
        struct ctl_table *entry = &WFQ_QDISC_Params_table[i];

        /* Initialize entry (ctl_table) */
        entry->procname = WFQ_QDISC_Params[i].name;
        entry->data = WFQ_QDISC_Params[i].ptr;
        entry->mode = 0644;

        /* WFQ_QDISC_DEBUG_MODE */
        if (i == 0)
        {
            entry->proc_handler = &proc_dointvec_minmax;
            entry->extra1 = &WFQ_QDISC_DEBUG_MODE_MIN;
            entry->extra2 = &WFQ_QDISC_DEBUG_MODE_MAX;
        }
        /* WFQ_QDISC_BUFFER_MODE */
        else if (i == 1)
        {
            entry->proc_handler = &proc_dointvec_minmax;
            entry->extra1 = &WFQ_QDISC_BUFFER_MODE_MIN;
            entry->extra2 = &WFQ_QDISC_BUFFER_MODE_MAX;
        }
        /* WFQ_QDISC_ECN_SCHEME */
        else if (i == 5)
        {
            entry->proc_handler = &proc_dointvec_minmax;
            entry->extra1 = &WFQ_QDISC_ECN_SCHEME_MIN;
            entry->extra2 = &WFQ_QDISC_ECN_SCHEME_MAX;
        }
        /* WFQ_QDISC_QUEUE_DSCP[] */
        else if (i >= 6 + WFQ_QDISC_MAX_QUEUES && i < 6 + 2 * WFQ_QDISC_MAX_QUEUES)
        {
            entry->proc_handler = &proc_dointvec_minmax;
            entry->extra1 = &WFQ_QDISC_DSCP_MIN;
            entry->extra2 = &WFQ_QDISC_DSCP_MAX;
        }
        /* WFQ_QDISC_QUEUE_WEIGHT[] */
        else if (i >= 6 + 2 * WFQ_QDISC_MAX_QUEUES && i < 6 + 3 * WFQ_QDISC_MAX_QUEUES)
        {
            entry->proc_handler = &proc_dointvec_minmax;
            entry->extra1 = &WFQ_QDISC_WEIGHT_MIN;
            entry->extra2 = &WFQ_QDISC_WEIGHT_MAX;
        }
        /*WFQ_QDISC_QUEUE_ECN_THRESH[] and WFQ_QDISC_QUEUE_BUFFER_BYTES[] */
        else
        {
            entry->proc_handler = &proc_dointvec;
        }
        entry->maxlen=sizeof(int);
    }

    WFQ_QDISC_Sysctl = register_sysctl_paths(WFQ_QDISC_Params_path, WFQ_QDISC_Params_table);

    if (likely(WFQ_QDISC_Sysctl))
        return 0;
    else
        return -1;
}

void wfq_qdisc_params_exit()
{
	if (likely(WFQ_QDISC_Sysctl))
		unregister_sysctl_table(WFQ_QDISC_Sysctl);
}
