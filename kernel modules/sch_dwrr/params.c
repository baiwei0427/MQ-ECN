#include "params.h"
#include <linux/sysctl.h>
#include <linux/string.h>

/* Debug mode or not. By default, we disable debug mode */
int DWRR_DEBUG_MODE = DWRR_DEBUG_OFF;
/* Buffer management mode: shared (0) or static (1). By default, we enable shread buffer. */
int DWRR_BUFFER_MODE = DWRR_SHARED_BUFFER;
/* Per port shared buffer (bytes) */
int DWRR_SHARED_BUFFER_BYTES = 96000;
/* Bucket size in bytes. By default, we use 2.5KB for 1G network. */
int DWRR_BUCKET_BYTES = 2500;
/* Per port ECN marking threshold (bytes). By default, we use 32KB for 1G network. */
int DWRR_PORT_THRESH_BYTES = 32000;
/* ECN marking scheme. By default, we use per queue ECN. */
int DWRR_ECN_SCHEME = DWRR_QUEUE_ECN;
/* Alpha for quantum sum estimation. It is 0.75 (768/1024) by default. */
int DWRR_QUANTUM_ALPHA = 768;
/* Alpha for round time estimation. It is 0.75 (768/1024) by default. */
int DWRR_ROUND_ALPHA = 768;
/* Idle time slot. It is 12us by default */
int DWRR_IDLE_INTERVAL_NS = 12000;
/* TCN threshold (1024 nanoseconds) */
int DWRR_TCN_THRESH = 250;
/* CoDel target (1024 nanoseconds) */
int DWRR_CODEL_TARGET = 250;
/* CoDel interval (1024 nanoseconds) */
int DWRR_CODEL_INTERVAL = 625;

int DWRR_DEBUG_MODE_MIN = DWRR_DEBUG_OFF;
int DWRR_DEBUG_MODE_MAX = DWRR_DEBUG_ON;
int DWRR_BUFFER_MODE_MIN = DWRR_SHARED_BUFFER;
int DWRR_BUFFER_MODE_MAX = DWRR_STATIC_BUFFER;
int DWRR_ECN_SCHEME_MIN = DWRR_DISABLE_ECN;
int DWRR_ECN_SCHEME_MAX = DWRR_CODEL;
int DWRR_QUANTUM_ALPHA_MIN = 0;
int DWRR_QUANTUM_ALPHA_MAX = (1 << 10);
int DWRR_ROUND_ALPHA_MIN = 0;
int DWRR_ROUND_ALPHA_MAX = (1 << 10);
int DWRR_DSCP_MIN = 0;
int DWRR_DSCP_MAX = 63;
int DWRR_QUANTUM_MIN = DWRR_MAX_PKT_BYTES;
int DWRR_QUANTUM_MAX = (200 << 10);

/* Per queue ECN marking threshold (bytes) */
int DWRR_QUEUE_THRESH_BYTES[DWRR_MAX_QUEUES];
/* DSCP value for different queues*/
int DWRR_QUEUE_DSCP[DWRR_MAX_QUEUES];
/* Quantum for different queues*/
int DWRR_QUEUE_QUANTUM[DWRR_MAX_QUEUES];
/* Per queue minimum guarantee buffer (bytes) */
int DWRR_QUEUE_BUFFER_BYTES[DWRR_MAX_QUEUES];

/* All parameters that can be configured through sysctl. We have DWRR_GLOBAL_PARAMS + 4 * DWRR_MAX_QUEUES parameters in total. */
struct dwrr_param DWRR_PARAMS[DWRR_GLOBAL_PARAMS + 4 * DWRR_MAX_QUEUES + 1] =
{
	{"debug_mode",		&DWRR_DEBUG_MODE},
	{"buffer_mode",		&DWRR_BUFFER_MODE},
	{"shared_buffer_bytes",	&DWRR_SHARED_BUFFER_BYTES},
	{"bucket_bytes",	&DWRR_BUCKET_BYTES},
	{"port_thresh_bytes",	&DWRR_PORT_THRESH_BYTES},
	{"ecn_scheme",		&DWRR_ECN_SCHEME},
	{"round_alpha",		&DWRR_ROUND_ALPHA},
	{"quantum_alpha",	&DWRR_QUANTUM_ALPHA},
	{"idle_interval_ns",	&DWRR_IDLE_INTERVAL_NS},
	{"tcn_thresh",		&DWRR_TCN_THRESH},
	{"codel_target",	&DWRR_CODEL_TARGET},
	{"codel_interval",	&DWRR_CODEL_INTERVAL},
};

struct ctl_table DWRR_PARAMS_TABLE[DWRR_GLOBAL_PARAMS + 4 * DWRR_MAX_QUEUES + 1];

struct ctl_path DWRR_PARAMS_PATH[] =
{
	{ .procname = "dwrr" },
	{ },
};

struct ctl_table_header *DWRR_SYSCTL = NULL;

bool dwrr_params_init()
{
	int i = 0;
	memset(DWRR_PARAMS_TABLE, 0, sizeof(DWRR_PARAMS_TABLE));

	for (i = 0; i < DWRR_MAX_QUEUES; i++)
	{
		/* Initialize DWRR_QUEUE_THRESH_BYTES[DWRR_MAX_QUEUES]*/
		snprintf(DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i].name,
			 63, "queue_thresh_bytes_%d", i);
		DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i].ptr = &DWRR_QUEUE_THRESH_BYTES[i];
		DWRR_QUEUE_THRESH_BYTES[i] = DWRR_PORT_THRESH_BYTES;

		/* Initialize DWRR_QUEUE_DSCP[DWRR_MAX_QUEUES] */
		snprintf(DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i + DWRR_MAX_QUEUES].name,
			 63, "queue_dscp_%d", i);
		DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i + DWRR_MAX_QUEUES].ptr = &DWRR_QUEUE_DSCP[i];
		DWRR_QUEUE_DSCP[i] = i;

		/* Initialize DWRR_QUEUE_QUANTUM[DWRR_MAX_QUEUES] */
		snprintf(DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i + 2 * DWRR_MAX_QUEUES].name,
			 63, "queue_quantum_%d", i);
		DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i + 2 * DWRR_MAX_QUEUES].ptr = &DWRR_QUEUE_QUANTUM[i];
		DWRR_QUEUE_QUANTUM[i] = DWRR_MAX_PKT_BYTES;

		/* Initialize DWRR_QUEUE_BUFFER_BYTES[DWRR_MAX_QUEUES] */
		snprintf(DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i + 3 * DWRR_MAX_QUEUES].name,
			 63, "queue_buffer_bytes_%d", i);
		DWRR_PARAMS[DWRR_GLOBAL_PARAMS + i + 3 * DWRR_MAX_QUEUES].ptr = &DWRR_QUEUE_BUFFER_BYTES[i];
		DWRR_QUEUE_BUFFER_BYTES[i] = DWRR_MAX_BUFFER_BYTES;
	}

	/* End of the parameters */
	DWRR_PARAMS[DWRR_GLOBAL_PARAMS + 4 * DWRR_MAX_QUEUES].ptr = NULL;

	for (i = 0; i < DWRR_GLOBAL_PARAMS + 4 * DWRR_MAX_QUEUES + 1; i++)
	{
		struct ctl_table *entry = &DWRR_PARAMS_TABLE[i];

		/* End */
		if (!DWRR_PARAMS[i].ptr)
			break;

		/* Initialize entry (ctl_table) */
		entry->procname = DWRR_PARAMS[i].name;
		entry->data = DWRR_PARAMS[i].ptr;
		entry->mode = 0644;

		/* DWRR_DEBUG_MODE */
		if (i == 0)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_DEBUG_MODE_MIN;
			entry->extra2 = &DWRR_DEBUG_MODE_MAX;
		}
		/* DWRR_BUFFER_MODE */
		else if (i == 1)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_BUFFER_MODE_MIN;
			entry->extra2 = &DWRR_BUFFER_MODE_MAX;
		}
		/* DWRR_ECN_SCHEME */
		else if (i == 5)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_ECN_SCHEME_MIN;
			entry->extra2 = &DWRR_ECN_SCHEME_MAX;
		}
		/* DWRR_QUANTUM_ALPHA*/
		else if (i == 6)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QUANTUM_ALPHA_MIN;
			entry->extra2 = &DWRR_QUANTUM_ALPHA_MAX;
		}
		/* DWRR_ROUND_ALPHA */
		else if (i == 7)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_ROUND_ALPHA_MIN;
			entry->extra2 = &DWRR_ROUND_ALPHA_MAX;
		}
		/* DWRR_QUEUE_DSCP[] */
		else if (i >= DWRR_GLOBAL_PARAMS + DWRR_MAX_QUEUES &&
			 i < DWRR_GLOBAL_PARAMS + 2 * DWRR_MAX_QUEUES)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_DSCP_MIN;
			entry->extra2 = &DWRR_DSCP_MAX;
		}
		/* DWRR_QUEUE_QUANTUM[] */
		else if (i >= DWRR_GLOBAL_PARAMS + 2 * DWRR_MAX_QUEUES &&
			 i < DWRR_GLOBAL_PARAMS + 3 * DWRR_MAX_QUEUES)
		{
			entry->proc_handler = &proc_dointvec_minmax;
			entry->extra1 = &DWRR_QUANTUM_MIN;
			entry->extra2 = &DWRR_QUANTUM_MAX;
		}
		/*DWRR_QUEUE_ECN_THRESH[] and DWRR_QUEUE_BUFFER_BYTES[] */
		else
		{
			entry->proc_handler = &proc_dointvec;
		}
		entry->maxlen=sizeof(int);
	}

	DWRR_SYSCTL = register_sysctl_paths(DWRR_PARAMS_PATH, DWRR_PARAMS_TABLE);
	if (likely(DWRR_SYSCTL))
		return true;
	else
		return false;

}

void dwrr_params_exit()
{
	if (likely(DWRR_SYSCTL))
		unregister_sysctl_table(DWRR_SYSCTL);
}
