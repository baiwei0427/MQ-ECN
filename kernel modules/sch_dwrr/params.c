#include "params.h"
#include <linux/sysctl.h>
#include <linux/string.h>

/* Debug mode or not. By default, we disable debug mode */
int DWRR_QDISC_DEBUG_MODE = DWRR_QDISC_DEBUG_OFF;
/* Buffer management mode: shared (0) or static (1). By default, we enable shread buffer. */
int DWRR_QDISC_BUFFER_MODE = DWRR_QDISC_SHARED_BUFFER;
/* Per port shared buffer (bytes) */
int DWRR_QDISC_SHARED_BUFFER_BYTES = 96 * 1024;
/* Bucket size in bytes. By default, we use 2.5KB for 1G network. */
int DWRR_QDISC_BUCKET_BYTES = 2500;
/* Per port ECN marking threshold (bytes). By default, we use 32KB for 1G network. */
int DWRR_QDISC_PORT_THRESH_BYTES = 32 * 1024;
/* ECN marking scheme. By default, we use per queue ECN. */
int DWRR_QDISC_ECN_SCHEME = DWRR_QDISC_QUEUE_ECN;
/* Alpha for quantum sum estimation. It is 0.75 (768/1024) by default. */
int DWRR_QDISC_QUANTUM_ALPHA = 768;
/* Alpha for round time estimation. It is 0.75 (768/1024) by default. */
int DWRR_QDISC_ROUND_ALPHA = 768;
/* Idle time slot. It is 12us by default */
int DWRR_QDISC_IDLE_INTERVAL_NS = 12000;
/* TCN threshold (1024 nanoseconds) */
int DWRR_QDISC_TCN_THRESH = 256;
/* CoDel target (1024 nanoseconds) */
int DWRR_QDISC_CODEL_TARGET = 128;
/* CoDel interval (1024 nanoseconds) */
int DWRR_QDISC_CODEL_INTERVAL = 1024;

int DWRR_QDISC_DEBUG_MODE_MIN = DWRR_QDISC_DEBUG_OFF;
int DWRR_QDISC_DEBUG_MODE_MAX = DWRR_QDISC_DEBUG_ON;
int DWRR_QDISC_BUFFER_MODE_MIN = DWRR_QDISC_SHARED_BUFFER;
int DWRR_QDISC_BUFFER_MODE_MAX = DWRR_QDISC_STATIC_BUFFER;
int DWRR_QDISC_ECN_SCHEME_MIN = DWRR_QDISC_DISABLE_ECN;
int DWRR_QDISC_ECN_SCHEME_MAX = DWRR_QDISC_CODEL;
int DWRR_QDISC_QUANTUM_ALPHA_MIN = 0;
int DWRR_QDISC_QUANTUM_ALPHA_MAX = (1 << 10);
int DWRR_QDISC_ROUND_ALPHA_MIN = 0;
int DWRR_QDISC_ROUND_ALPHA_MAX = (1 << 10);
int DWRR_QDISC_DSCP_MIN = 0;
int DWRR_QDISC_DSCP_MAX = 63;
int DWRR_QDISC_QUANTUM_MIN = DWRR_QDISC_MTU_BYTES;
int DWRR_QDISC_QUANTUM_MAX = (200 << 10);

/* Per queue ECN marking threshold (bytes) */
int DWRR_QDISC_QUEUE_THRESH_BYTES[DWRR_QDISC_MAX_QUEUES];
/* DSCP value for different queues*/
int DWRR_QDISC_QUEUE_DSCP[DWRR_QDISC_MAX_QUEUES];
/* Quantum for different queues*/
int DWRR_QDISC_QUEUE_QUANTUM[DWRR_QDISC_MAX_QUEUES];
/* Per queue minimum guarantee buffer (bytes) */
int DWRR_QDISC_QUEUE_BUFFER_BYTES[DWRR_QDISC_MAX_QUEUES];

/* All parameters that can be configured through sysctl. We have DWRR_QDISC_NUM_GLOBAL_PARAMS + 4 * DWRR_QDISC_MAX_QUEUES parameters in total. */
struct dwrr_qdisc_param DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + 4 * DWRR_QDISC_MAX_QUEUES + 1] =
{
	{"debug_mode", &DWRR_QDISC_DEBUG_MODE},
	{"buffer_mode", &DWRR_QDISC_BUFFER_MODE},
	{"shared_buffer_bytes", &DWRR_QDISC_SHARED_BUFFER_BYTES},
	{"bucket_bytes", &DWRR_QDISC_BUCKET_BYTES},
	{"port_thresh_bytes", &DWRR_QDISC_PORT_THRESH_BYTES},
	{"ecn_scheme", &DWRR_QDISC_ECN_SCHEME},
	{"round_alpha", &DWRR_QDISC_ROUND_ALPHA},
	{"quantum_alpha", &DWRR_QDISC_QUANTUM_ALPHA},
	{"idle_interval_ns", &DWRR_QDISC_IDLE_INTERVAL_NS},
	{"tcn_thresh", &DWRR_QDISC_TCN_THRESH},
	{"codel_target", &DWRR_QDISC_CODEL_TARGET},
	{"codel_interval", &DWRR_QDISC_CODEL_INTERVAL},
};

struct ctl_table DWRR_QDISC_PARAMS_TABLE[DWRR_QDISC_NUM_GLOBAL_PARAMS + 4 * DWRR_QDISC_MAX_QUEUES + 1];

struct ctl_path DWRR_QDISC_PARAMS_PATH[] =
{
	{ .procname = "dwrr" },
	{ },
};

struct ctl_table_header *DWRR_QDISC_SYSCTL = NULL;

bool dwrr_qdisc_params_init()
{
	int i = 0;
	memset(DWRR_QDISC_PARAMS_TABLE, 0, sizeof(DWRR_QDISC_PARAMS_TABLE));

	for (i = 0; i < DWRR_QDISC_MAX_QUEUES; i++)
	{
		/* Initialize DWRR_QDISC_QUEUE_THRESH_BYTES[DWRR_QDISC_MAX_QUEUES]*/
		snprintf(DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i].name, 63,"queue_thresh_bytes_%d", i);
		DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i].ptr = &DWRR_QDISC_QUEUE_THRESH_BYTES[i];
		DWRR_QDISC_QUEUE_THRESH_BYTES[i] = DWRR_QDISC_PORT_THRESH_BYTES;

		/* Initialize DWRR_QDISC_QUEUE_DSCP[DWRR_QDISC_MAX_QUEUES] */
		snprintf(DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i + DWRR_QDISC_MAX_QUEUES].name, 63, "queue_dscp_%d", i);
		DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i + DWRR_QDISC_MAX_QUEUES].ptr = &DWRR_QDISC_QUEUE_DSCP[i];
		DWRR_QDISC_QUEUE_DSCP[i] = i;

		/* Initialize DWRR_QDISC_QUEUE_QUANTUM[DWRR_QDISC_MAX_QUEUES] */
		snprintf(DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i + 2 * DWRR_QDISC_MAX_QUEUES].name, 63, "queue_quantum_%d", i);
		DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i + 2 * DWRR_QDISC_MAX_QUEUES].ptr = &DWRR_QDISC_QUEUE_QUANTUM[i];
		DWRR_QDISC_QUEUE_QUANTUM[i] = DWRR_QDISC_MTU_BYTES;

		/* Initialize DWRR_QDISC_QUEUE_BUFFER_BYTES[DWRR_QDISC_MAX_QUEUES] */
		snprintf(DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i + 3 * DWRR_QDISC_MAX_QUEUES].name, 63, "queue_buffer_bytes_%d", i);
		DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + i + 3 * DWRR_QDISC_MAX_QUEUES].ptr = &DWRR_QDISC_QUEUE_BUFFER_BYTES[i];
		DWRR_QDISC_QUEUE_BUFFER_BYTES[i] = DWRR_QDISC_MAX_BUFFER_BYTES;
	}

	/* End of the parameters */
	DWRR_QDISC_PARAMS[DWRR_QDISC_NUM_GLOBAL_PARAMS + 4 * DWRR_QDISC_MAX_QUEUES].ptr = NULL;

	for (i = 0; i < DWRR_QDISC_NUM_GLOBAL_PARAMS + 4 * DWRR_QDISC_MAX_QUEUES + 1; i++)
	{
		struct ctl_table *entry = &DWRR_QDISC_PARAMS_TABLE[i];

		/* End */
		if (!DWRR_QDISC_PARAMS[i].ptr)
			break;

		/* Initialize entry (ctl_table) */
		entry->procname = DWRR_QDISC_PARAMS[i].name;
		entry->data = DWRR_QDISC_PARAMS[i].ptr;
		entry->mode = 0644;

		/* DWRR_QDISC_DEBUG_MODE */
		if (i == 0)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QDISC_DEBUG_MODE_MIN;
			entry->extra2 = &DWRR_QDISC_DEBUG_MODE_MAX;
		}
		/* DWRR_QDISC_BUFFER_MODE */
		else if (i == 1)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QDISC_BUFFER_MODE_MIN;
			entry->extra2 = &DWRR_QDISC_BUFFER_MODE_MAX;
		}
		/* DWRR_QDISC_ECN_SCHEME */
		else if (i == 5)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QDISC_ECN_SCHEME_MIN;
			entry->extra2 = &DWRR_QDISC_ECN_SCHEME_MAX;
		}
		/* DWRR_QDISC_QUANTUM_ALPHA*/
		else if (i == 6)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QDISC_QUANTUM_ALPHA_MIN;
			entry->extra2 = &DWRR_QDISC_QUANTUM_ALPHA_MAX;
		}
		/* DWRR_QDISC_ROUND_ALPHA */
		else if (i == 7)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QDISC_ROUND_ALPHA_MIN;
			entry->extra2 = &DWRR_QDISC_ROUND_ALPHA_MAX;
		}
		/* DWRR_QDISC_QUEUE_DSCP[] */
		else if (i >= DWRR_QDISC_NUM_GLOBAL_PARAMS + DWRR_QDISC_MAX_QUEUES && i < DWRR_QDISC_NUM_GLOBAL_PARAMS + 2 * DWRR_QDISC_MAX_QUEUES)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QDISC_DSCP_MIN;
			entry->extra2 = &DWRR_QDISC_DSCP_MAX;
		}
		/* DWRR_QDISC_QUEUE_QUANTUM[] */
		else if (i >= DWRR_QDISC_NUM_GLOBAL_PARAMS + 2 * DWRR_QDISC_MAX_QUEUES && i < DWRR_QDISC_NUM_GLOBAL_PARAMS + 3 * DWRR_QDISC_MAX_QUEUES)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QDISC_QUANTUM_MIN;
			entry->extra2 = &DWRR_QDISC_QUANTUM_MAX;
		}
		/*DWRR_QDISC_QUEUE_ECN_THRESH[] and DWRR_QDISC_QUEUE_BUFFER_BYTES[] */
		else
		{
			entry->proc_handler = &proc_dointvec;
		}
		entry->maxlen=sizeof(int);
	}

	DWRR_QDISC_SYSCTL = register_sysctl_paths(DWRR_QDISC_PARAMS_PATH, DWRR_QDISC_PARAMS_TABLE);
	if (likely(DWRR_QDISC_SYSCTL))
		return true;
	else
		return false;

}

void dwrr_qdisc_params_exit()
{
	if (likely(DWRR_QDISC_SYSCTL))
		unregister_sysctl_table(DWRR_QDISC_SYSCTL);
}
