/*
 * HEVC video Decoder
 *
 * Copyright (C) 2012 - 2013 Guillaume Martres
 * Copyright (C) 2012 - 2013 Mickael Raulet
 * Copyright (C) 2012 - 2013 Gildas Cocherel
 * Copyright (C) 2012 - 2013 Wassim Hamidouche
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdbool.h>

#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sched.h>
#include <unistd.h>

#include "libavutil/attributes.h"
#include "libavutil/common.h"
#include "libavutil/display.h"
#include "libavutil/internal.h"
#include "libavutil/mastering_display_metadata.h"
#include "libavutil/md5.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/stereo3d.h"

#include "bswapdsp.h"
#include "bytestream.h"
#include "cabac_functions.h"
#include "golomb.h"
#include "hevc.h"
#include "hevc_data.h"
#include "hevcdec.h"
#include "profiles.h"

#ifdef RPI
  #include "rpi_qpu.h"
  #include "rpi_shader.h"
  #include "rpi_shader_cmd.h"
  #include "rpi_shader_template.h"
  #include "rpi_zc.h"
  #include "libavutil/rpi_sand_fns.h"

  // Define RPI_CACHE_UNIF_MVS to write motion vector uniform stream to cached memory
  #define RPI_CACHE_UNIF_MVS  1

  // Define RPI_SIMULATE_QPUS for debugging to run QPU code on the ARMs (*rotted*)
  //#define RPI_SIMULATE_QPUS
  #ifdef RPI_WORKER
    #include "pthread.h"
  #endif

  #include "libavutil/atomic.h"

  static void worker_core(HEVCContext * const s);
#endif

#define DEBUG_DECODE_N 0   // 0 = do all, n = frames idr onwards

#define PACK2(hi,lo) (((hi) << 16) | ((lo) & 0xffff))

#ifndef av_mod_uintp2
static av_always_inline av_const unsigned av_mod_uintp2_c(unsigned a, unsigned p)
{
    return a & ((1 << p) - 1);
}
#   define av_mod_uintp2   av_mod_uintp2_c
#endif

const uint8_t ff_hevc_pel_weight[65] = { [2] = 0, [4] = 1, [6] = 2, [8] = 3, [12] = 4, [16] = 5, [24] = 6, [32] = 7, [48] = 8, [64] = 9 };


#if RPI_INTER

#define MC_DUMMY_X (-32)
#define MC_DUMMY_Y (-32)

// UV still has min 4x4 pred
// Allow for even spread +1 for setup, +1 for rounding
// As we have load sharing this can (in theory) be exceeded so we have to
// check after each CTU, but it is a good base size

// Worst case (all 4x4) commands per CTU
#define QPU_Y_CMD_PER_CTU_MAX (8 * 8)
#define QPU_C_CMD_PER_CTU_MAX (4 * 4)

#define QPU_C_COMMANDS (((RPI_MAX_WIDTH * 64) / (4 * 4)) / 4 + 2 * QPU_N_MAX)
#define QPU_Y_COMMANDS (((RPI_MAX_WIDTH * 64) / (4 * 4))     + 2 * QPU_N_MAX)

// The QPU code for UV blocks only works up to a block width of 8
#define RPI_CHROMA_BLOCK_WIDTH 8

#define ENCODE_COEFFS(c0, c1, c2, c3) (((c0) & 0xff) | ((c1) & 0xff) << 8 | ((c2) & 0xff) << 16 | ((c3) & 0xff) << 24)


// Actual filter goes -ve, +ve, +ve, -ve using these values
static const uint32_t rpi_filter_coefs[8] = {
        ENCODE_COEFFS(  0,  64,   0,  0),
        ENCODE_COEFFS(  2,  58,  10,  2),
        ENCODE_COEFFS(  4,  54,  16,  2),
        ENCODE_COEFFS(  6,  46,  28,  4),
        ENCODE_COEFFS(  4,  36,  36,  4),
        ENCODE_COEFFS(  4,  28,  46,  6),
        ENCODE_COEFFS(  2,  16,  54,  4),
        ENCODE_COEFFS(  2,  10,  58,  2)
};

// Function arrays by QPU

static const int * const inter_pred_setup_c_qpu[12] = {
    mc_setup_c_q0, mc_setup_c_qn, mc_setup_c_qn, mc_setup_c_qn,
    mc_setup_c_qn, mc_setup_c_qn, mc_setup_c_qn, mc_setup_c_qn,
    mc_setup_c_qn, mc_setup_c_qn, mc_setup_c_qn, mc_setup_c_qn
};

static const int * const inter_pred_setup_c10_qpu[12] = {
    mc_setup_c10_q0, mc_setup_c10_qn, mc_setup_c10_qn, mc_setup_c10_qn,
    mc_setup_c10_qn, mc_setup_c10_qn, mc_setup_c10_qn, mc_setup_c10_qn,
    mc_setup_c10_qn, mc_setup_c10_qn, mc_setup_c10_qn, mc_setup_c10_qn
};

static const int * const inter_pred_setup_y_qpu[12] = {
    mc_setup_y_q0, mc_setup_y_qn, mc_setup_y_qn, mc_setup_y_qn,
    mc_setup_y_qn, mc_setup_y_qn, mc_setup_y_qn, mc_setup_y_qn,
    mc_setup_y_qn, mc_setup_y_qn, mc_setup_y_qn, mc_setup_y_qn
};

static const int * const inter_pred_setup_y10_qpu[12] = {
    mc_setup_y10_q0, mc_setup_y10_qn, mc_setup_y10_qn, mc_setup_y10_qn,
    mc_setup_y10_qn, mc_setup_y10_qn, mc_setup_y10_qn, mc_setup_y10_qn,
    mc_setup_y10_qn, mc_setup_y10_qn, mc_setup_y10_qn, mc_setup_y10_qn
};

static const int * const inter_pred_sync_qpu[12] = {
    mc_sync_q0, mc_sync_q1, mc_sync_q2, mc_sync_q3,
    mc_sync_q4, mc_sync_q5, mc_sync_q6, mc_sync_q7,
    mc_sync_q8, mc_sync_q9, mc_sync_q10, mc_sync_q11
};

static const int * const inter_pred_sync10_qpu[12] = {
    mc_sync10_q0, mc_sync10_q1, mc_sync10_q2, mc_sync10_q3,
    mc_sync10_q4, mc_sync10_q5, mc_sync10_q6, mc_sync10_q7,
    mc_sync10_q8, mc_sync10_q9, mc_sync10_q10, mc_sync10_q11
};

static const int * const inter_pred_exit_c_qpu[12] = {
    mc_exit_c_q0, mc_exit_c_qn, mc_exit_c_qn, mc_exit_c_qn,
    mc_exit_c_qn, mc_exit_c_qn, mc_exit_c_qn, mc_exit_c_qn,
    mc_exit_c_qn, mc_exit_c_qn, mc_exit_c_qn, mc_exit_c_qn
};

static const int * const inter_pred_exit_c10_qpu[12] = {
    mc_exit_c10_q0, mc_exit_c10_qn, mc_exit_c10_qn, mc_exit_c10_qn,
    mc_exit_c10_qn, mc_exit_c10_qn, mc_exit_c10_qn, mc_exit_c10_qn,
    mc_exit_c10_qn, mc_exit_c10_qn, mc_exit_c10_qn, mc_exit_c10_qn
};

static const int * const inter_pred_exit_y_qpu[12] = {
    mc_exit_y_q0, mc_exit_y_qn, mc_exit_y_qn, mc_exit_y_qn,
    mc_exit_y_qn, mc_exit_y_qn, mc_exit_y_qn, mc_exit_y_qn,
    mc_exit_y_qn, mc_exit_y_qn, mc_exit_y_qn, mc_exit_y_qn
};

static const int * const inter_pred_exit_y10_qpu[12] = {
    mc_exit_y10_q0, mc_exit_y10_qn, mc_exit_y10_qn, mc_exit_y10_qn,
    mc_exit_y10_qn, mc_exit_y10_qn, mc_exit_y10_qn, mc_exit_y10_qn,
    mc_exit_y10_qn, mc_exit_y10_qn, mc_exit_y10_qn, mc_exit_y10_qn
};

typedef struct ipe_chan_info_s
{
    const unsigned int n;
    const int * const * setup_fns;
    const int * const * sync_fns;
    const int * const * exit_fns;
} ipe_chan_info_t;

typedef struct ipe_init_info_s
{
    ipe_chan_info_t luma;
    ipe_chan_info_t chroma;
} ipe_init_info_t;

static const ipe_init_info_t ipe_init_infos[9] = {  // Alloc for bit depths of 8-16
   {  // 8
      .luma =   {QPU_MC_PRED_N_Y8, inter_pred_setup_y_qpu, inter_pred_sync_qpu, inter_pred_exit_y_qpu},
      .chroma = {QPU_MC_PRED_N_C8, inter_pred_setup_c_qpu, inter_pred_sync_qpu, inter_pred_exit_c_qpu}
   },
   {  // 9
      .luma =   {0},
      .chroma = {0}
   },
   {  // 10
      .luma =   {QPU_MC_PRED_N_Y10, inter_pred_setup_y10_qpu, inter_pred_sync10_qpu, inter_pred_exit_y10_qpu},
      .chroma = {QPU_MC_PRED_N_C10, inter_pred_setup_c10_qpu, inter_pred_sync10_qpu, inter_pred_exit_c10_qpu}
   }

};

static void set_ipe_from_ici(HEVCRpiInterPredEnv * const ipe, const ipe_chan_info_t * const ici)
{
    const unsigned int n = ici->n;
    const unsigned int q1_size = (ipe->gptr.numbytes / n) & ~3;  // Round down to word

    ipe->n = n;
    ipe->max_fill = q1_size - ipe->min_gap;
    for(unsigned int i = 0; i < n; i++) {
        HEVCRpiInterPredQ * const q = ipe->q + i;
        q->qpu_mc_curr = q->qpu_mc_base =
            (qpu_mc_pred_cmd_t *)(ipe->gptr.arm + i * q1_size);
        q->code_setup = qpu_fn(ici->setup_fns[i]);
        q->code_sync = qpu_fn(ici->sync_fns[i]);
        q->code_exit = qpu_fn(ici->exit_fns[i]);
    }
}

static void rpi_hevc_qpu_set_fns(HEVCContext * const s, const unsigned int bit_depth)
{
    const ipe_init_info_t * const iii = ipe_init_infos + bit_depth - 8;

    av_assert0(bit_depth >= 8 && bit_depth <= 16);

    rpi_hevc_qpu_init_fn(&s->qpu, bit_depth);

    for (unsigned int i = 0; i != RPI_MAX_JOBS; ++i) {
        HEVCRpiJob *const jb = s->jobs + i;
        set_ipe_from_ici(&jb->chroma_ip, &iii->chroma);
        set_ipe_from_ici(&jb->luma_ip,   &iii->luma);
    }
}


#endif


static pthread_mutex_t threadlog_mutex = PTHREAD_MUTEX_INITIALIZER;

#define MAX_TIME (100*1000000)
#define MAX_SNAPSHOTS 1000000
#define MAX_THREADS 18

typedef struct thread_journal
{
    struct thread_journal *next;
    threadlog_thread_t type;
    pthread_t self;
    unsigned state_idx;
} thread_journal_t;
static struct
{
    unsigned last_thread_start_time;
    bool finished;
    thread_journal_t *thread;
    unsigned state_idx;
    unsigned snapshot_idx;
    struct
    {
        unsigned time;
        unsigned char state[MAX_THREADS];
    } snapshot[MAX_SNAPSHOTS];
} journal;

static unsigned gettime_us(void)
{
    struct timeval now;
    gettimeofday(&now, 0);
    return now.tv_sec * 1000000 + now.tv_usec;
}

static void threadlog_final(void)
{
    struct tms tms;
    times(&tms);

    static unsigned real_time,
                    running_time,
                    wait_worker[2],
                    wait_job_slot,
                    wait_eof_join,
                    wait_progress,
                    wait_gpu,
                    runnable[21],
                    cumulative_runnable[21];

    int i = 0;
    unsigned last_period = 0;
    while ((int) (journal.snapshot[i].time - journal.last_thread_start_time) < 0)
    {
        i++;
    }
    while (i < journal.snapshot_idx)
    {
        int runnable_threads = 0;
        if (i > 0)
            last_period = journal.snapshot[i].time - journal.snapshot[i-1].time;
        for (thread_journal_t *t = journal.thread; t != NULL; t = t->next)
        {
            switch (journal.snapshot[i].state[t->state_idx])
            {
            case threadlog_reason_RUNNING:
            case threadlog_reason_RUNNING_REF_CODING_TREE:
            case threadlog_reason_RUNNING_SETUP:
            case threadlog_reason_RUNNING_FLUSH:
            case threadlog_reason_RUNNING_INTRA_PRED:
            case threadlog_reason_RUNNING_DEBLOCK:
                running_time += last_period;
                runnable_threads++;
                break;
            case threadlog_reason_WAIT_FRAME_WORKER_THREAD:
                wait_worker[0] += last_period;
                break;
            case threadlog_reason_WAIT_SECONDARY_WORKER:
                wait_worker[1] += last_period;
                break;
            case threadlog_reason_PASS0_JOB_SLOT:
                wait_job_slot += last_period;
                break;
            case threadlog_reason_END_OF_FRAME_JOIN:
                wait_eof_join += last_period;
                break;
            case threadlog_reason_AWAIT_PROGRESS_PRED_L0:
            case threadlog_reason_AWAIT_PROGRESS_PRED_L1:
            case threadlog_reason_AWAIT_PROGRESS_MV_BOTTOM_RIGHT:
            case threadlog_reason_AWAIT_PROGRESS_MV_CENTRE:
            case threadlog_reason_AWAIT_PROGRESS_OTHER:
                wait_progress += last_period;
                break;
            case threadlog_reason_WAIT_QPU:
                wait_gpu += last_period;
                break;
            }
        }
        real_time += last_period;
        runnable[runnable_threads] += last_period;
        for (int t = 0; t <= runnable_threads; t++)
            cumulative_runnable[t] += last_period;
        i++;
    }
    FILE *f = fopen("ffmpeg-analysis.csv", "a");
    if (f)
    {
        fprintf(f, "%f, %f, %f, %f, %f, %f, %f, %f, %f, %f, %f",
                 tms.tms_utime / 100.,
                 tms.tms_stime / 100.,
                 (tms.tms_utime + tms.tms_stime) / 100.,
                 real_time / 1000000.,
                 running_time / 1000000.,
                 wait_worker[0] / 1000000.,
                 wait_worker[1] / 1000000.,
                 wait_job_slot / 1000000.,
                 wait_eof_join / 1000000.,
                 wait_progress / 1000000.,
                 wait_gpu / 1000000.);
        for (i = 0; i < sizeof runnable / sizeof *runnable; i++)
            fprintf(f, ", %f", runnable[i] / 1000000.);
        for (i = 0; i < sizeof runnable / sizeof *runnable; i++)
            fprintf(f, ", %f", cumulative_runnable[i] / 1000000.);
        fprintf(f, "\n");
        fclose(f);
    }
}

__thread threadlog_reason_t threadlog_progress_type = threadlog_reason_AWAIT_PROGRESS_OTHER;

void threadlog_init(void)
{
	atexit(threadlog_final);
}

void threadlog_done(void)
{
    journal.finished = true;
}

static __thread threadlog_reason_t threadlog_state_stack[4];
static __thread threadlog_reason_t *threadlog_state_sp;

void threadlog_thread_start(int thread_type)
{
    threadlog_state_sp = threadlog_state_stack;
    *threadlog_state_sp++ = threadlog_reason_RUNNING;

    pthread_mutex_lock(&threadlog_mutex);

    if (!journal.finished)
    {
        thread_journal_t *thread;
        if (journal.state_idx == MAX_THREADS || (thread = malloc(sizeof *thread)) == NULL)
            fprintf(stderr, "Warning: thread limit exceeded!\n");
        else
        {
            /* Store in list, sorted by thread_type, at end of threads with matching thread_type */
            thread_journal_t *prev = NULL;
            if (journal.thread != NULL && journal.thread->type <= thread_type)
            {
                prev = journal.thread;
                while (prev->next != NULL && prev->next->type <= thread_type)
                    prev = prev->next;
                thread->next = prev->next;
                prev->next = thread;
            }
            else
            {
                thread->next = journal.thread;
                journal.thread = thread;
            }
            thread->type = thread_type;
            thread->self = pthread_self();
            thread->state_idx = journal.state_idx++;
            journal.last_thread_start_time = gettime_us();
        }
    }

    pthread_mutex_unlock(&threadlog_mutex);
}

void threadlog_thread_end(int thread_type)
{
    pthread_mutex_lock(&threadlog_mutex);

    if (!journal.finished)
    {
        /* Ignore threads that bow out early */
        pthread_t self = pthread_self();
        thread_journal_t *prev = NULL, *thread = journal.thread;
        while (thread && thread->self != self)
        {
            prev = thread;
            thread = thread->next;
        }
        if (thread)
        {
            if (prev)
                prev->next = thread->next;
            else
                journal.thread = thread->next;
        }
    }

    pthread_mutex_unlock(&threadlog_mutex);
}

void threadlog_update(int sleep_reason, int change)
{
	pthread_mutex_lock(&threadlog_mutex);

    if (change > 0)
        *++threadlog_state_sp = sleep_reason;
    else
        --threadlog_state_sp;

    if (!journal.finished)
    {
        pthread_t self = pthread_self();
        thread_journal_t *thread = journal.thread;
        while (thread && thread->self != self)
            thread = thread->next;
        if (thread)
        {
            if (journal.snapshot_idx > 0)
                memcpy(journal.snapshot[journal.snapshot_idx].state, journal.snapshot[journal.snapshot_idx - 1].state, sizeof journal.snapshot[0].state);
            journal.snapshot[journal.snapshot_idx].state[thread->state_idx] = *threadlog_state_sp;
            journal.snapshot[journal.snapshot_idx].time = gettime_us();
            if (journal.snapshot[journal.snapshot_idx].time - journal.last_thread_start_time > MAX_TIME)
                journal.finished = true;
            if (++journal.snapshot_idx == MAX_SNAPSHOTS)
            {
                fprintf(stderr, "Warning: snapshot limit exceeded!\n");
                journal.finished = true;
            }
        }
    }

	pthread_mutex_unlock(&threadlog_mutex);
}

void threadlog_timer_enable(int enable)
{
	if (!enable)
	{
		for (int i = 0; i < threadlog_reason_MAX; i++)
			threadlog_update(i, 0);
	}
}





#ifdef RPI_WORKER

//#define LOG_ENTER printf("Enter %s: p0=%d p1=%d (%d jobs) %p\n", __func__,s->pass0_job,s->pass1_job,s->worker_tail-s->worker_head,s);
//#define LOG_EXIT printf("Exit %s: p0=%d p1=%d (%d jobs) %p\n", __func__,s->pass0_job,s->pass1_job,s->worker_tail-s->worker_head,s);

#define LOG_ENTER
#define LOG_EXIT

#define USE_SEM 1

// Call this when we have completed pass0 and wish to trigger pass1 for the current job
static void worker_submit_job(HEVCContext * const s)
{
    LOG_ENTER
    sem_post(&s->jb0->sem_in);
    s->jb0->pending = 1;
    s->pass0_job = (s->pass0_job + 1) % RPI_MAX_JOBS; // Move onto the next slot
    s->jb0 = s->jobs + s->pass0_job;
    LOG_EXIT
}

// Call this to say we have completed pass1
static void worker_complete_job(HEVCContext * const s)
{
    LOG_ENTER
    sem_t * const sem = &s->jb1->sem_out;
    // Must set job no before signalling as otherwise rpi_do_all_passes
    // may call worker_core from the main thread with a bad job number
    s->pass1_job = (s->pass1_job + 1) % RPI_MAX_JOBS; // Move onto the next slot
    s->jb1 = s->jobs + s->pass1_job;
    sem_post(sem);
    LOG_EXIT
}


// Call worker_pass0_ready to wait until the s->pass0_job slot becomes
// available to receive the next job.
static void worker_pass0_ready(HEVCContext *s)
{
    LOG_ENTER
    HEVCRpiJob * const jb = s->jb0;
    if (jb->pending) {
        threadlog_update(threadlog_reason_PASS0_JOB_SLOT, +1);
        while (sem_wait(&jb->sem_out) == -1 && errno == EINTR)
            /* Loop */;
        threadlog_update(threadlog_reason_PASS0_JOB_SLOT, -1);
        jb->pending = 0;
    }
    LOG_EXIT
}

// Call this to wait for all jobs to have completed at the end of a frame
static void worker_wait(HEVCContext * const s)
{
    LOG_ENTER
    unsigned int i;
    for (i = 0; i != RPI_MAX_JOBS; ++i) {
        HEVCRpiJob * const jb = s->jobs + i;
        if (jb->pending) {
            threadlog_update(threadlog_reason_END_OF_FRAME_JOIN, +1);
            while (sem_wait(&jb->sem_out) == -1 && errno == EINTR)
                /* Loop */;
            threadlog_update(threadlog_reason_END_OF_FRAME_JOIN, -1);
            jb->pending = 0;
        }
    }
    LOG_EXIT
}

static void *worker_start(void *arg)
{
    HEVCContext * const s = (HEVCContext *)arg;
    s->avctx->internal->worker_tid[1] = syscall(SYS_gettid);

    threadlog_thread_start(threadlog_thread_SECONDARY);

    for (;;)
    {
        HEVCRpiJob * const jb = s->jb1;
        threadlog_update(threadlog_reason_WAIT_SECONDARY_WORKER, +1);
        while (sem_wait(&jb->sem_in) == -1 && errno == EINTR)
            /* Loop */;
        threadlog_update(threadlog_reason_WAIT_SECONDARY_WORKER, -1);
        if (jb->terminate)
            break;

        LOG_ENTER
        worker_core(s);
        worker_complete_job(s);
        LOG_EXIT
    }
    threadlog_thread_end(threadlog_thread_SECONDARY);
    return NULL;
}

#endif

static void worker_pic_free_all(HEVCContext * const s)
{
    unsigned int i;

    // Free coeff stuff - allocation not the same for all buffers
    for(i = 0; i < RPI_MAX_JOBS; i++)
    {
        HEVCRpiCoeffsEnv * const cf = &s->jobs[i].coeffs;

        if (cf->s[0].buf != NULL)
            av_freep(&cf->mptr);
        if (cf->s[2].buf != NULL)
            gpu_free(&cf->gptr);
        memset(cf, 0, sizeof(*cf));
    }
}

static int worker_pic_alloc_all(HEVCContext * const s, const unsigned int coeff_count)
{
    unsigned int i;

    // Free coeff stuff - allocation not the same for all buffers
    for(i = 0; i < RPI_MAX_JOBS; i++)
    {
        HEVCRpiCoeffsEnv * const cf = &s->jobs[i].coeffs;

//        av_assert0(cf->s[0].n == 0 && cf->s[0].buf == NULL);
//        av_assert0(cf->s[1].n == 0 && cf->s[1].buf == NULL);
//        av_assert0(cf->s[2].n == 0 && cf->s[2].buf == NULL);
//        av_assert0(cf->s[3].n == 0 && cf->s[3].buf == NULL);

        if (gpu_malloc_cached((coeff_count + 32*32) * sizeof(cf->s[2].buf[0]), &cf->gptr) != 0)
            goto fail;
        cf->s[2].buf = (int16_t *)cf->gptr.arm;
        cf->s[3].buf = cf->s[2].buf + coeff_count;

        // Must be 64 byte aligned for our zero apping code so over-allocate &
        // round
        if ((cf->mptr = av_malloc(coeff_count * sizeof(cf->s[0].buf[0] + 63))) == NULL)
            goto fail;
        cf->s[0].buf = (void *)(((intptr_t)cf->mptr + 63) & ~63);
    }
    return 0;

fail:
    printf("%s: **** Failed\n", __func__);
    worker_pic_free_all(s);
    return -1;
}

static void worker_pic_reset(HEVCRpiCoeffsEnv * const cf)
{
    unsigned int i;
    for (i = 0; i != 4; ++i) {
        cf->s[i].n = 0;
    }
}


/**
 * NOTE: Each function hls_foo correspond to the function foo in the
 * specification (HLS stands for High Level Syntax).
 */

/**
 * Section 5.7
 */

/* free everything allocated  by pic_arrays_init() */
static void pic_arrays_free(HEVCContext *s)
{
    worker_pic_free_all(s);

#ifdef RPI_DEBLOCK_VPU
    {
        int i;
        for (i = 0; i != RPI_DEBLOCK_VPU_Q_COUNT; ++i) {
            struct dblk_vpu_q_s * const dvq = s->dvq_ents + i;

            if (dvq->vpu_cmds_arm) {
                gpu_free(&dvq->deblock_vpu_gmem);
              dvq->vpu_cmds_arm = 0;
            }
        }
    }
#endif
    av_freep(&s->sao);
    av_freep(&s->deblock);

    av_freep(&s->skip_flag);
    av_freep(&s->tab_ct_depth);

    av_freep(&s->tab_ipm);
    av_freep(&s->cbf_luma);
    av_freep(&s->is_pcm);

    av_freep(&s->qp_y_tab);
    av_freep(&s->tab_slice_address);
    av_freep(&s->filter_slice_edges);

    av_freep(&s->horizontal_bs);
    av_freep(&s->vertical_bs);

    av_freep(&s->sh.entry_point_offset);
    av_freep(&s->sh.size);
    av_freep(&s->sh.offset);

    av_buffer_pool_uninit(&s->tab_mvf_pool);
    av_buffer_pool_uninit(&s->rpl_tab_pool);
}

/* allocate arrays that depend on frame dimensions */
static int pic_arrays_init(HEVCContext *s, const HEVCSPS *sps)
{
    int log2_min_cb_size = sps->log2_min_cb_size;
    int width            = sps->width;
    int height           = sps->height;
    int pic_size_in_ctb  = ((width  >> log2_min_cb_size) + 1) *
                           ((height >> log2_min_cb_size) + 1);
    int ctb_count        = sps->ctb_width * sps->ctb_height;
    int min_pu_size      = sps->min_pu_width * sps->min_pu_height;

#ifdef RPI
    const int coefs_in_ctb = (1 << sps->log2_ctb_size) * (1 << sps->log2_ctb_size);
    const int coefs_per_luma = 64*64*RPI_CHUNK_SIZE*RPI_NUM_CHUNKS;
    const int coefs_per_chroma = (coefs_per_luma * 2) >> sps->vshift[1] >> sps->hshift[1];
    const int coefs_per_row = coefs_per_luma + coefs_per_chroma;

    av_assert0(sps);
    s->max_ctu_count = FFMIN(coefs_per_luma / coefs_in_ctb, sps->ctb_width);

    if (worker_pic_alloc_all(s, coefs_per_row) != 0)
        goto fail;
#endif
#ifdef RPI_DEBLOCK_VPU
    {
        int i;
        s->enable_rpi_deblock = !sps->sao_enabled;
        s->setup_width = (sps->width+15) / 16;
        s->setup_height = (sps->height+15) / 16;
        s->uv_setup_width = ( (sps->width >> sps->hshift[1]) + 15) / 16;
        s->uv_setup_height = ( (sps->height >> sps->vshift[1]) + 15) / 16;

        for (i = 0; i != RPI_DEBLOCK_VPU_Q_COUNT; ++i)
        {
            struct dblk_vpu_q_s * const dvq = s->dvq_ents + i;
            const unsigned int cmd_size = (sizeof(*dvq->vpu_cmds_arm) * 3 + 15) & ~15;
            const unsigned int y_size = (sizeof(*dvq->y_setup_arm) * s->setup_width * s->setup_height + 15) & ~15;
            const unsigned int uv_size = (sizeof(*dvq->uv_setup_arm) * s->uv_setup_width * s->uv_setup_height + 15) & ~15;
            const unsigned int total_size =- cmd_size + y_size + uv_size;
            int p_vc;
            uint8_t * p_arm;
 #if RPI_VPU_DEBLOCK_CACHED
            gpu_malloc_cached(total_size, &dvq->deblock_vpu_gmem);
 #else
            gpu_malloc_uncached(total_size, &dvq->deblock_vpu_gmem);
 #endif
            p_vc = dvq->deblock_vpu_gmem.vc;
            p_arm = dvq->deblock_vpu_gmem.arm;

            // Zap all
            memset(p_arm, 0, dvq->deblock_vpu_gmem.numbytes);

            // Subdivide
            dvq->vpu_cmds_arm = (void*)p_arm;
            dvq->vpu_cmds_vc = p_vc;

            p_arm += cmd_size;
            p_vc += cmd_size;

            dvq->y_setup_arm = (void*)p_arm;
            dvq->y_setup_vc = (void*)p_vc;

            p_arm += y_size;
            p_vc += y_size;

            dvq->uv_setup_arm = (void*)p_arm;
            dvq->uv_setup_vc = (void*)p_vc;
        }

        s->dvq_n = 0;
        s->dvq = s->dvq_ents + s->dvq_n;
    }
#endif

    s->bs_width  = (width  >> 2) + 1;
    s->bs_height = (height >> 2) + 1;

    s->sao           = av_mallocz_array(ctb_count, sizeof(*s->sao));
    s->deblock       = av_mallocz_array(ctb_count, sizeof(*s->deblock));
    if (!s->sao || !s->deblock)
        goto fail;

    s->skip_flag    = av_malloc_array(sps->min_cb_height, sps->min_cb_width);
    s->tab_ct_depth = av_malloc_array(sps->min_cb_height, sps->min_cb_width);
    if (!s->skip_flag || !s->tab_ct_depth)
        goto fail;

    s->cbf_luma = av_malloc_array(sps->min_tb_width, sps->min_tb_height);
    s->tab_ipm  = av_mallocz(min_pu_size);
    s->is_pcm   = av_malloc_array(sps->min_pu_width + 1, sps->min_pu_height + 1);
    if (!s->tab_ipm || !s->cbf_luma || !s->is_pcm)
        goto fail;

    s->filter_slice_edges = av_mallocz(ctb_count);
    s->tab_slice_address  = av_malloc_array(pic_size_in_ctb,
                                      sizeof(*s->tab_slice_address));
    s->qp_y_tab           = av_malloc_array(pic_size_in_ctb,
                                      sizeof(*s->qp_y_tab));
    if (!s->qp_y_tab || !s->filter_slice_edges || !s->tab_slice_address)
        goto fail;

    s->horizontal_bs = av_mallocz_array(s->bs_width, s->bs_height);
    s->vertical_bs   = av_mallocz_array(s->bs_width, s->bs_height);
    if (!s->horizontal_bs || !s->vertical_bs)
        goto fail;

    s->tab_mvf_pool = av_buffer_pool_init(min_pu_size * sizeof(MvField),
                                          av_buffer_allocz);
    s->rpl_tab_pool = av_buffer_pool_init(ctb_count * sizeof(RefPicListTab),
                                          av_buffer_allocz);
    if (!s->tab_mvf_pool || !s->rpl_tab_pool)
        goto fail;

    return 0;

fail:
    pic_arrays_free(s);
    return AVERROR(ENOMEM);
}

static void default_pred_weight_table(HEVCContext * const s)
{
  unsigned int i;
  s->sh.luma_log2_weight_denom = 0;
  s->sh.chroma_log2_weight_denom = 0;
  for (i = 0; i < s->sh.nb_refs[L0]; i++) {
      s->sh.luma_weight_l0[i] = 1;
      s->sh.luma_offset_l0[i] = 0;
      s->sh.chroma_weight_l0[i][0] = 1;
      s->sh.chroma_offset_l0[i][0] = 0;
      s->sh.chroma_weight_l0[i][1] = 1;
      s->sh.chroma_offset_l0[i][1] = 0;
  }
  for (i = 0; i < s->sh.nb_refs[L1]; i++) {
      s->sh.luma_weight_l1[i] = 1;
      s->sh.luma_offset_l1[i] = 0;
      s->sh.chroma_weight_l1[i][0] = 1;
      s->sh.chroma_offset_l1[i][0] = 0;
      s->sh.chroma_weight_l1[i][1] = 1;
      s->sh.chroma_offset_l1[i][1] = 0;
  }
}

static void pred_weight_table(HEVCContext *s, GetBitContext *gb)
{
    int i = 0;
    int j = 0;
    uint8_t luma_weight_l0_flag[16];
    uint8_t chroma_weight_l0_flag[16];
    uint8_t luma_weight_l1_flag[16];
    uint8_t chroma_weight_l1_flag[16];
    int luma_log2_weight_denom;

    luma_log2_weight_denom = get_ue_golomb_long(gb);
    if (luma_log2_weight_denom < 0 || luma_log2_weight_denom > 7)
        av_log(s->avctx, AV_LOG_ERROR, "luma_log2_weight_denom %d is invalid\n", luma_log2_weight_denom);
    s->sh.luma_log2_weight_denom = av_clip_uintp2(luma_log2_weight_denom, 3);
    if (s->ps.sps->chroma_format_idc != 0) {
        int delta = get_se_golomb(gb);
        s->sh.chroma_log2_weight_denom = av_clip_uintp2(s->sh.luma_log2_weight_denom + delta, 3);
    }

    for (i = 0; i < s->sh.nb_refs[L0]; i++) {
        luma_weight_l0_flag[i] = get_bits1(gb);
        if (!luma_weight_l0_flag[i]) {
            s->sh.luma_weight_l0[i] = 1 << s->sh.luma_log2_weight_denom;
            s->sh.luma_offset_l0[i] = 0;
        }
    }
    if (s->ps.sps->chroma_format_idc != 0) {
        for (i = 0; i < s->sh.nb_refs[L0]; i++)
            chroma_weight_l0_flag[i] = get_bits1(gb);
    } else {
        for (i = 0; i < s->sh.nb_refs[L0]; i++)
            chroma_weight_l0_flag[i] = 0;
    }
    for (i = 0; i < s->sh.nb_refs[L0]; i++) {
        if (luma_weight_l0_flag[i]) {
            int delta_luma_weight_l0 = get_se_golomb(gb);
            s->sh.luma_weight_l0[i] = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l0;
            s->sh.luma_offset_l0[i] = get_se_golomb(gb);
        }
        if (chroma_weight_l0_flag[i]) {
            for (j = 0; j < 2; j++) {
                int delta_chroma_weight_l0 = get_se_golomb(gb);
                int delta_chroma_offset_l0 = get_se_golomb(gb);
                s->sh.chroma_weight_l0[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l0;
                s->sh.chroma_offset_l0[i][j] = av_clip((delta_chroma_offset_l0 - ((128 * s->sh.chroma_weight_l0[i][j])
                                                                                    >> s->sh.chroma_log2_weight_denom) + 128), -128, 127);
            }
        } else {
            s->sh.chroma_weight_l0[i][0] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][0] = 0;
            s->sh.chroma_weight_l0[i][1] = 1 << s->sh.chroma_log2_weight_denom;
            s->sh.chroma_offset_l0[i][1] = 0;
        }
    }
    if (s->sh.slice_type == HEVC_SLICE_B) {
        for (i = 0; i < s->sh.nb_refs[L1]; i++) {
            luma_weight_l1_flag[i] = get_bits1(gb);
            if (!luma_weight_l1_flag[i]) {
                s->sh.luma_weight_l1[i] = 1 << s->sh.luma_log2_weight_denom;
                s->sh.luma_offset_l1[i] = 0;
            }
        }
        if (s->ps.sps->chroma_format_idc != 0) {
            for (i = 0; i < s->sh.nb_refs[L1]; i++)
                chroma_weight_l1_flag[i] = get_bits1(gb);
        } else {
            for (i = 0; i < s->sh.nb_refs[L1]; i++)
                chroma_weight_l1_flag[i] = 0;
        }
        for (i = 0; i < s->sh.nb_refs[L1]; i++) {
            if (luma_weight_l1_flag[i]) {
                int delta_luma_weight_l1 = get_se_golomb(gb);
                s->sh.luma_weight_l1[i] = (1 << s->sh.luma_log2_weight_denom) + delta_luma_weight_l1;
                s->sh.luma_offset_l1[i] = get_se_golomb(gb);
            }
            if (chroma_weight_l1_flag[i]) {
                for (j = 0; j < 2; j++) {
                    int delta_chroma_weight_l1 = get_se_golomb(gb);
                    int delta_chroma_offset_l1 = get_se_golomb(gb);
                    s->sh.chroma_weight_l1[i][j] = (1 << s->sh.chroma_log2_weight_denom) + delta_chroma_weight_l1;
                    s->sh.chroma_offset_l1[i][j] = av_clip((delta_chroma_offset_l1 - ((128 * s->sh.chroma_weight_l1[i][j])
                                                                                        >> s->sh.chroma_log2_weight_denom) + 128), -128, 127);
                }
            } else {
                s->sh.chroma_weight_l1[i][0] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][0] = 0;
                s->sh.chroma_weight_l1[i][1] = 1 << s->sh.chroma_log2_weight_denom;
                s->sh.chroma_offset_l1[i][1] = 0;
            }
        }
    }
}

static int decode_lt_rps(HEVCContext *s, LongTermRPS *rps, GetBitContext *gb)
{
    const HEVCSPS *sps = s->ps.sps;
    int max_poc_lsb    = 1 << sps->log2_max_poc_lsb;
    int prev_delta_msb = 0;
    unsigned int nb_sps = 0, nb_sh;
    int i;

    rps->nb_refs = 0;
    if (!sps->long_term_ref_pics_present_flag)
        return 0;

    if (sps->num_long_term_ref_pics_sps > 0)
        nb_sps = get_ue_golomb_long(gb);
    nb_sh = get_ue_golomb_long(gb);

    if (nb_sh + (uint64_t)nb_sps > FF_ARRAY_ELEMS(rps->poc))
        return AVERROR_INVALIDDATA;

    rps->nb_refs = nb_sh + nb_sps;

    for (i = 0; i < rps->nb_refs; i++) {
        uint8_t delta_poc_msb_present;

        if (i < nb_sps) {
            uint8_t lt_idx_sps = 0;

            if (sps->num_long_term_ref_pics_sps > 1)
                lt_idx_sps = get_bits(gb, av_ceil_log2(sps->num_long_term_ref_pics_sps));

            rps->poc[i]  = sps->lt_ref_pic_poc_lsb_sps[lt_idx_sps];
            rps->used[i] = sps->used_by_curr_pic_lt_sps_flag[lt_idx_sps];
        } else {
            rps->poc[i]  = get_bits(gb, sps->log2_max_poc_lsb);
            rps->used[i] = get_bits1(gb);
        }

        delta_poc_msb_present = get_bits1(gb);
        if (delta_poc_msb_present) {
            int delta = get_ue_golomb_long(gb);

            if (i && i != nb_sps)
                delta += prev_delta_msb;

            rps->poc[i] += s->poc - delta * max_poc_lsb - s->sh.pic_order_cnt_lsb;
            prev_delta_msb = delta;
        }
    }

    return 0;
}

static void export_stream_params(AVCodecContext *avctx, const HEVCParamSets *ps,
                                 const HEVCSPS *sps)
{
    const HEVCVPS *vps = (const HEVCVPS*)ps->vps_list[sps->vps_id]->data;
    unsigned int num = 0, den = 0;

    avctx->pix_fmt             = sps->pix_fmt;
    avctx->coded_width         = sps->width;
    avctx->coded_height        = sps->height;
    avctx->width               = sps->output_width;
    avctx->height              = sps->output_height;
    avctx->has_b_frames        = sps->temporal_layer[sps->max_sub_layers - 1].num_reorder_pics;
    avctx->profile             = sps->ptl.general_ptl.profile_idc;
    avctx->level               = sps->ptl.general_ptl.level_idc;

    ff_set_sar(avctx, sps->vui.sar);

    if (sps->vui.video_signal_type_present_flag)
        avctx->color_range = sps->vui.video_full_range_flag ? AVCOL_RANGE_JPEG
                                                            : AVCOL_RANGE_MPEG;
    else
        avctx->color_range = AVCOL_RANGE_MPEG;

    if (sps->vui.colour_description_present_flag) {
        avctx->color_primaries = sps->vui.colour_primaries;
        avctx->color_trc       = sps->vui.transfer_characteristic;
        avctx->colorspace      = sps->vui.matrix_coeffs;
    } else {
        avctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
        avctx->color_trc       = AVCOL_TRC_UNSPECIFIED;
        avctx->colorspace      = AVCOL_SPC_UNSPECIFIED;
    }

    if (vps->vps_timing_info_present_flag) {
        num = vps->vps_num_units_in_tick;
        den = vps->vps_time_scale;
    } else if (sps->vui.vui_timing_info_present_flag) {
        num = sps->vui.vui_num_units_in_tick;
        den = sps->vui.vui_time_scale;
    }

    if (num != 0 && den != 0)
        av_reduce(&avctx->framerate.den, &avctx->framerate.num,
                  num, den, 1 << 30);
}

static int set_sps(HEVCContext *s, const HEVCSPS *sps, enum AVPixelFormat pix_fmt)
{
    #define HWACCEL_MAX (CONFIG_HEVC_DXVA2_HWACCEL + CONFIG_HEVC_D3D11VA_HWACCEL + CONFIG_HEVC_VAAPI_HWACCEL + CONFIG_HEVC_VDPAU_HWACCEL)
    enum AVPixelFormat pix_fmts[HWACCEL_MAX + 4], *fmt = pix_fmts;
    int ret;

    pic_arrays_free(s);
    s->ps.sps = NULL;
    s->ps.vps = NULL;

    if (!sps)
        return 0;

    ret = pic_arrays_init(s, sps);
    if (ret < 0)
        goto fail;

    export_stream_params(s->avctx, &s->ps, sps);

    switch (sps->pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
#if RPI_HEVC_SAND
        // Currently geometry calc is stuffed for big sizes
        if (sps->width < 2048 && sps->height <= 1088) {
            *fmt++ = AV_PIX_FMT_SAND128;
        }
#endif
#if CONFIG_HEVC_DXVA2_HWACCEL
        *fmt++ = AV_PIX_FMT_DXVA2_VLD;
#endif
#if CONFIG_HEVC_D3D11VA_HWACCEL
        *fmt++ = AV_PIX_FMT_D3D11VA_VLD;
#endif
#if CONFIG_HEVC_VAAPI_HWACCEL
        *fmt++ = AV_PIX_FMT_VAAPI;
#endif
#if CONFIG_HEVC_VDPAU_HWACCEL
        *fmt++ = AV_PIX_FMT_VDPAU;
#endif
        break;
    case AV_PIX_FMT_YUV420P10:
#if RPI_HEVC_SAND
        // Currently geometry calc is stuffed for big sizes
        if (sps->width < 2048 && sps->height <= 1088) {
            *fmt++ = AV_PIX_FMT_SAND64_10;
        }
#endif
#if CONFIG_HEVC_DXVA2_HWACCEL
        *fmt++ = AV_PIX_FMT_DXVA2_VLD;
#endif
#if CONFIG_HEVC_D3D11VA_HWACCEL
        *fmt++ = AV_PIX_FMT_D3D11VA_VLD;
#endif
#if CONFIG_HEVC_VAAPI_HWACCEL
        *fmt++ = AV_PIX_FMT_VAAPI;
#endif
        break;
    }

    if (pix_fmt == AV_PIX_FMT_NONE) {
        *fmt++ = sps->pix_fmt;
        *fmt = AV_PIX_FMT_NONE;

        ret = ff_thread_get_format(s->avctx, pix_fmts);
        if (ret < 0)
            goto fail;

        s->avctx->pix_fmt = ret;
    }
    else {
        s->avctx->pix_fmt = pix_fmt;
    }

    ff_hevc_pred_init(&s->hpc,     sps->bit_depth);
    ff_hevc_dsp_init (&s->hevcdsp, sps->bit_depth);
    ff_videodsp_init (&s->vdsp,    sps->bit_depth);
#ifdef RPI
    rpi_hevc_qpu_set_fns(s, sps->bit_depth);
#endif

    av_freep(&s->sao_pixel_buffer_h[0]);
    av_freep(&s->sao_pixel_buffer_v[0]);

    if (sps->sao_enabled && !s->avctx->hwaccel) {
        const unsigned int c_count = (sps->chroma_format_idc != 0) ? 3 : 1;
        unsigned int c_idx;
        size_t vsize[3] = {0};
        size_t hsize[3] = {0};

        for(c_idx = 0; c_idx < c_count; c_idx++) {
            int w = sps->width >> sps->hshift[c_idx];
            int h = sps->height >> sps->vshift[c_idx];
            // ctb height & width are a min of 8 so this must a multiple of 16
            // so no point rounding up!
            hsize[c_idx] = (w * 2 * sps->ctb_height) << sps->pixel_shift;
            vsize[c_idx] = (h * 2 * sps->ctb_width) << sps->pixel_shift;
        }

        // Allocate as a single lump so we can extend h[1] & v[1] into h[2] & v[2]
        // when we have plaited chroma
        s->sao_pixel_buffer_h[0] = av_malloc(hsize[0] + hsize[1] + hsize[2]);
        s->sao_pixel_buffer_v[0] = av_malloc(vsize[0] + vsize[1] + vsize[2]);
        s->sao_pixel_buffer_h[1] = s->sao_pixel_buffer_h[0] + hsize[0];
        s->sao_pixel_buffer_h[2] = s->sao_pixel_buffer_h[1] + hsize[1];
        s->sao_pixel_buffer_v[1] = s->sao_pixel_buffer_v[0] + vsize[0];
        s->sao_pixel_buffer_v[2] = s->sao_pixel_buffer_v[1] + vsize[1];
    }

    s->ps.sps = sps;
    s->ps.vps = (HEVCVPS*) s->ps.vps_list[s->ps.sps->vps_id]->data;

    return 0;

fail:
    pic_arrays_free(s);
    s->ps.sps = NULL;
    return ret;
}

static int hls_slice_header(HEVCContext *s)
{
    GetBitContext *gb = &s->HEVClc->gb;
    SliceHeader *sh   = &s->sh;
    int i, ret;

    // Coded parameters
    sh->first_slice_in_pic_flag = get_bits1(gb);
    if ((IS_IDR(s) || IS_BLA(s)) && sh->first_slice_in_pic_flag) {
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
        if (IS_IDR(s))
            ff_hevc_clear_refs(s);
    }
    sh->no_output_of_prior_pics_flag = 0;
    if (IS_IRAP(s))
        sh->no_output_of_prior_pics_flag = get_bits1(gb);

    sh->pps_id = get_ue_golomb_long(gb);
    if (sh->pps_id >= HEVC_MAX_PPS_COUNT || !s->ps.pps_list[sh->pps_id]) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS id out of range: %d\n", sh->pps_id);
        return AVERROR_INVALIDDATA;
    }
    if (!sh->first_slice_in_pic_flag &&
        s->ps.pps != (HEVCPPS*)s->ps.pps_list[sh->pps_id]->data) {
        av_log(s->avctx, AV_LOG_ERROR, "PPS changed between slices.\n");
        return AVERROR_INVALIDDATA;
    }
    s->ps.pps = (HEVCPPS*)s->ps.pps_list[sh->pps_id]->data;
    if (s->nal_unit_type == HEVC_NAL_CRA_NUT && s->last_eos == 1)
        sh->no_output_of_prior_pics_flag = 1;

    if (s->ps.sps != (HEVCSPS*)s->ps.sps_list[s->ps.pps->sps_id]->data) {
        const HEVCSPS* last_sps = s->ps.sps;
        s->ps.sps = (HEVCSPS*)s->ps.sps_list[s->ps.pps->sps_id]->data;
        if (last_sps && IS_IRAP(s) && s->nal_unit_type != HEVC_NAL_CRA_NUT) {
            if (s->ps.sps->width !=  last_sps->width || s->ps.sps->height != last_sps->height ||
                s->ps.sps->temporal_layer[s->ps.sps->max_sub_layers - 1].max_dec_pic_buffering !=
                last_sps->temporal_layer[last_sps->max_sub_layers - 1].max_dec_pic_buffering)
                sh->no_output_of_prior_pics_flag = 0;
        }
        ff_hevc_clear_refs(s);
        ret = set_sps(s, s->ps.sps, AV_PIX_FMT_NONE);
        if (ret < 0)
            return ret;

        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
    }

    sh->dependent_slice_segment_flag = 0;
    if (!sh->first_slice_in_pic_flag) {
        int slice_address_length;

        if (s->ps.pps->dependent_slice_segments_enabled_flag)
            sh->dependent_slice_segment_flag = get_bits1(gb);

        slice_address_length = av_ceil_log2(s->ps.sps->ctb_width *
                                            s->ps.sps->ctb_height);
        sh->slice_segment_addr = get_bitsz(gb, slice_address_length);
        if (sh->slice_segment_addr >= s->ps.sps->ctb_width * s->ps.sps->ctb_height) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Invalid slice segment address: %u.\n",
                   sh->slice_segment_addr);
            return AVERROR_INVALIDDATA;
        }

        if (!sh->dependent_slice_segment_flag) {
            sh->slice_addr = sh->slice_segment_addr;
            s->slice_idx++;
        }
    } else {
        sh->slice_segment_addr = sh->slice_addr = 0;
        s->slice_idx           = 0;
        s->slice_initialized   = 0;
    }

    if (!sh->dependent_slice_segment_flag) {
        s->slice_initialized = 0;

        for (i = 0; i < s->ps.pps->num_extra_slice_header_bits; i++)
            skip_bits(gb, 1);  // slice_reserved_undetermined_flag[]

        sh->slice_type = get_ue_golomb_long(gb);
        if (!(sh->slice_type == HEVC_SLICE_I ||
              sh->slice_type == HEVC_SLICE_P ||
              sh->slice_type == HEVC_SLICE_B)) {
            av_log(s->avctx, AV_LOG_ERROR, "Unknown slice type: %d.\n",
                   sh->slice_type);
            return AVERROR_INVALIDDATA;
        }
        if (IS_IRAP(s) && sh->slice_type != HEVC_SLICE_I) {
            av_log(s->avctx, AV_LOG_ERROR, "Inter slices in an IRAP frame.\n");
            return AVERROR_INVALIDDATA;
        }

        // when flag is not present, picture is inferred to be output
        sh->pic_output_flag = 1;
        if (s->ps.pps->output_flag_present_flag)
            sh->pic_output_flag = get_bits1(gb);

        if (s->ps.sps->separate_colour_plane_flag)
            sh->colour_plane_id = get_bits(gb, 2);

        if (!IS_IDR(s)) {
            int poc, pos;

            sh->pic_order_cnt_lsb = get_bits(gb, s->ps.sps->log2_max_poc_lsb);
            poc = ff_hevc_compute_poc(s, sh->pic_order_cnt_lsb);
            if (!sh->first_slice_in_pic_flag && poc != s->poc) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Ignoring POC change between slices: %d -> %d\n", s->poc, poc);
                if (s->avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
                poc = s->poc;
            }
            s->poc = poc;

            sh->short_term_ref_pic_set_sps_flag = get_bits1(gb);
            pos = get_bits_left(gb);
            if (!sh->short_term_ref_pic_set_sps_flag) {
                ret = ff_hevc_decode_short_term_rps(gb, s->avctx, &sh->slice_rps, s->ps.sps, 1);
                if (ret < 0)
                    return ret;

                sh->short_term_rps = &sh->slice_rps;
            } else {
                int numbits, rps_idx;

                if (!s->ps.sps->nb_st_rps) {
                    av_log(s->avctx, AV_LOG_ERROR, "No ref lists in the SPS.\n");
                    return AVERROR_INVALIDDATA;
                }

                numbits = av_ceil_log2(s->ps.sps->nb_st_rps);
                rps_idx = numbits > 0 ? get_bits(gb, numbits) : 0;
                sh->short_term_rps = &s->ps.sps->st_rps[rps_idx];
            }
            sh->short_term_ref_pic_set_size = pos - get_bits_left(gb);

            pos = get_bits_left(gb);
            ret = decode_lt_rps(s, &sh->long_term_rps, gb);
            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING, "Invalid long term RPS.\n");
                if (s->avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
            }
            sh->long_term_ref_pic_set_size = pos - get_bits_left(gb);

            if (s->ps.sps->sps_temporal_mvp_enabled_flag)
                sh->slice_temporal_mvp_enabled_flag = get_bits1(gb);
            else
                sh->slice_temporal_mvp_enabled_flag = 0;
        } else {
            s->sh.short_term_rps = NULL;
            s->poc               = 0;
        }

        /* 8.3.1 */
        if (sh->first_slice_in_pic_flag && s->temporal_id == 0 &&
            s->nal_unit_type != HEVC_NAL_TRAIL_N &&
            s->nal_unit_type != HEVC_NAL_TSA_N   &&
            s->nal_unit_type != HEVC_NAL_STSA_N  &&
            s->nal_unit_type != HEVC_NAL_RADL_N  &&
            s->nal_unit_type != HEVC_NAL_RADL_R  &&
            s->nal_unit_type != HEVC_NAL_RASL_N  &&
            s->nal_unit_type != HEVC_NAL_RASL_R)
            s->pocTid0 = s->poc;

        if (s->ps.sps->sao_enabled) {
            sh->slice_sample_adaptive_offset_flag[0] = get_bits1(gb);
            if (s->ps.sps->chroma_format_idc) {
                sh->slice_sample_adaptive_offset_flag[1] =
                sh->slice_sample_adaptive_offset_flag[2] = get_bits1(gb);
            }
        } else {
            sh->slice_sample_adaptive_offset_flag[0] = 0;
            sh->slice_sample_adaptive_offset_flag[1] = 0;
            sh->slice_sample_adaptive_offset_flag[2] = 0;
        }

        sh->nb_refs[L0] = sh->nb_refs[L1] = 0;
        if (sh->slice_type == HEVC_SLICE_P || sh->slice_type == HEVC_SLICE_B) {
            int nb_refs;

            sh->nb_refs[L0] = s->ps.pps->num_ref_idx_l0_default_active;
            if (sh->slice_type == HEVC_SLICE_B)
                sh->nb_refs[L1] = s->ps.pps->num_ref_idx_l1_default_active;

            if (get_bits1(gb)) { // num_ref_idx_active_override_flag
                sh->nb_refs[L0] = get_ue_golomb_long(gb) + 1;
                if (sh->slice_type == HEVC_SLICE_B)
                    sh->nb_refs[L1] = get_ue_golomb_long(gb) + 1;
            }
            if (sh->nb_refs[L0] > HEVC_MAX_REFS || sh->nb_refs[L1] > HEVC_MAX_REFS) {
                av_log(s->avctx, AV_LOG_ERROR, "Too many refs: %d/%d.\n",
                       sh->nb_refs[L0], sh->nb_refs[L1]);
                return AVERROR_INVALIDDATA;
            }

            sh->rpl_modification_flag[0] = 0;
            sh->rpl_modification_flag[1] = 0;
            nb_refs = ff_hevc_frame_nb_refs(s);
            if (!nb_refs) {
                av_log(s->avctx, AV_LOG_ERROR, "Zero refs for a frame with P or B slices.\n");
                return AVERROR_INVALIDDATA;
            }

            if (s->ps.pps->lists_modification_present_flag && nb_refs > 1) {
                sh->rpl_modification_flag[0] = get_bits1(gb);
                if (sh->rpl_modification_flag[0]) {
                    for (i = 0; i < sh->nb_refs[L0]; i++)
                        sh->list_entry_lx[0][i] = get_bits(gb, av_ceil_log2(nb_refs));
                }

                if (sh->slice_type == HEVC_SLICE_B) {
                    sh->rpl_modification_flag[1] = get_bits1(gb);
                    if (sh->rpl_modification_flag[1] == 1)
                        for (i = 0; i < sh->nb_refs[L1]; i++)
                            sh->list_entry_lx[1][i] = get_bits(gb, av_ceil_log2(nb_refs));
                }
            }

            if (sh->slice_type == HEVC_SLICE_B)
                sh->mvd_l1_zero_flag = get_bits1(gb);

            if (s->ps.pps->cabac_init_present_flag)
                sh->cabac_init_flag = get_bits1(gb);
            else
                sh->cabac_init_flag = 0;

            sh->collocated_ref_idx = 0;
            if (sh->slice_temporal_mvp_enabled_flag) {
                sh->collocated_list = L0;
                if (sh->slice_type == HEVC_SLICE_B)
                    sh->collocated_list = !get_bits1(gb);

                if (sh->nb_refs[sh->collocated_list] > 1) {
                    sh->collocated_ref_idx = get_ue_golomb_long(gb);
                    if (sh->collocated_ref_idx >= sh->nb_refs[sh->collocated_list]) {
                        av_log(s->avctx, AV_LOG_ERROR,
                               "Invalid collocated_ref_idx: %d.\n",
                               sh->collocated_ref_idx);
                        return AVERROR_INVALIDDATA;
                    }
                }
            }

            if ((s->ps.pps->weighted_pred_flag   && sh->slice_type == HEVC_SLICE_P) ||
                (s->ps.pps->weighted_bipred_flag && sh->slice_type == HEVC_SLICE_B)) {
                pred_weight_table(s, gb);
            }
            else
            {
              // Give us unit weights
              default_pred_weight_table(s);
            }

            sh->max_num_merge_cand = 5 - get_ue_golomb_long(gb);
            if (sh->max_num_merge_cand < 1 || sh->max_num_merge_cand > 5) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "Invalid number of merging MVP candidates: %d.\n",
                       sh->max_num_merge_cand);
                return AVERROR_INVALIDDATA;
            }
        }

        sh->slice_qp_delta = get_se_golomb(gb);

        if (s->ps.pps->pic_slice_level_chroma_qp_offsets_present_flag) {
            sh->slice_cb_qp_offset = get_se_golomb(gb);
            sh->slice_cr_qp_offset = get_se_golomb(gb);
        } else {
            sh->slice_cb_qp_offset = 0;
            sh->slice_cr_qp_offset = 0;
        }

        if (s->ps.pps->chroma_qp_offset_list_enabled_flag)
            sh->cu_chroma_qp_offset_enabled_flag = get_bits1(gb);
        else
            sh->cu_chroma_qp_offset_enabled_flag = 0;

        if (s->ps.pps->deblocking_filter_control_present_flag) {
            int deblocking_filter_override_flag = 0;

            if (s->ps.pps->deblocking_filter_override_enabled_flag)
                deblocking_filter_override_flag = get_bits1(gb);

            if (deblocking_filter_override_flag) {
                sh->disable_deblocking_filter_flag = get_bits1(gb);
                if (!sh->disable_deblocking_filter_flag) {
                    sh->beta_offset = get_se_golomb(gb) * 2;
                    sh->tc_offset   = get_se_golomb(gb) * 2;
                }
            } else {
                sh->disable_deblocking_filter_flag = s->ps.pps->disable_dbf;
                sh->beta_offset                    = s->ps.pps->beta_offset;
                sh->tc_offset                      = s->ps.pps->tc_offset;
            }
        } else {
            sh->disable_deblocking_filter_flag = 0;
            sh->beta_offset                    = 0;
            sh->tc_offset                      = 0;
        }

        if (s->ps.pps->seq_loop_filter_across_slices_enabled_flag &&
            (sh->slice_sample_adaptive_offset_flag[0] ||
             sh->slice_sample_adaptive_offset_flag[1] ||
             !sh->disable_deblocking_filter_flag)) {
            sh->slice_loop_filter_across_slices_enabled_flag = get_bits1(gb);
        } else {
            sh->slice_loop_filter_across_slices_enabled_flag = s->ps.pps->seq_loop_filter_across_slices_enabled_flag;
        }
    } else if (!s->slice_initialized) {
        av_log(s->avctx, AV_LOG_ERROR, "Independent slice segment missing.\n");
        return AVERROR_INVALIDDATA;
    }

    sh->num_entry_point_offsets = 0;
    if (s->ps.pps->tiles_enabled_flag || s->ps.pps->entropy_coding_sync_enabled_flag) {
        unsigned num_entry_point_offsets = get_ue_golomb_long(gb);
        // It would be possible to bound this tighter but this here is simpler
        if (num_entry_point_offsets > get_bits_left(gb)) {
            av_log(s->avctx, AV_LOG_ERROR, "num_entry_point_offsets %d is invalid\n", num_entry_point_offsets);
            return AVERROR_INVALIDDATA;
        }

        sh->num_entry_point_offsets = num_entry_point_offsets;
        if (sh->num_entry_point_offsets > 0) {
            int offset_len = get_ue_golomb_long(gb) + 1;

            if (offset_len < 1 || offset_len > 32) {
                sh->num_entry_point_offsets = 0;
                av_log(s->avctx, AV_LOG_ERROR, "offset_len %d is invalid\n", offset_len);
                return AVERROR_INVALIDDATA;
            }

            av_freep(&sh->entry_point_offset);
            av_freep(&sh->offset);
            av_freep(&sh->size);
            sh->entry_point_offset = av_malloc_array(sh->num_entry_point_offsets, sizeof(unsigned));
            sh->offset = av_malloc_array(sh->num_entry_point_offsets, sizeof(int));
            sh->size = av_malloc_array(sh->num_entry_point_offsets, sizeof(int));
            if (!sh->entry_point_offset || !sh->offset || !sh->size) {
                sh->num_entry_point_offsets = 0;
                av_log(s->avctx, AV_LOG_ERROR, "Failed to allocate memory\n");
                return AVERROR(ENOMEM);
            }
            for (i = 0; i < sh->num_entry_point_offsets; i++) {
                unsigned val = get_bits_long(gb, offset_len);
                sh->entry_point_offset[i] = val + 1; // +1; // +1 to get the size
            }
            if (s->threads_number > 1 && (s->ps.pps->num_tile_rows > 1 || s->ps.pps->num_tile_columns > 1)) {
                s->enable_parallel_tiles = 0; // TODO: you can enable tiles in parallel here
                s->threads_number = 1;
            } else
                s->enable_parallel_tiles = 0;
        } else
            s->enable_parallel_tiles = 0;
    }

    if (s->ps.pps->slice_header_extension_present_flag) {
        unsigned int length = get_ue_golomb_long(gb);
        if (length*8LL > get_bits_left(gb)) {
            av_log(s->avctx, AV_LOG_ERROR, "too many slice_header_extension_data_bytes\n");
            return AVERROR_INVALIDDATA;
        }
        for (i = 0; i < length; i++)
            skip_bits(gb, 8);  // slice_header_extension_data_byte
    }

    // Inferred parameters
    sh->slice_qp = 26U + s->ps.pps->pic_init_qp_minus26 + sh->slice_qp_delta;
    if (sh->slice_qp > 51 ||
        sh->slice_qp < -s->ps.sps->qp_bd_offset) {
        av_log(s->avctx, AV_LOG_ERROR,
               "The slice_qp %d is outside the valid range "
               "[%d, 51].\n",
               sh->slice_qp,
               -s->ps.sps->qp_bd_offset);
        return AVERROR_INVALIDDATA;
    }

    sh->slice_ctb_addr_rs = sh->slice_segment_addr;

    if (!s->sh.slice_ctb_addr_rs && s->sh.dependent_slice_segment_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Impossible slice segment.\n");
        return AVERROR_INVALIDDATA;
    }

    if (get_bits_left(gb) < 0) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Overread slice header by %d bits\n", -get_bits_left(gb));
        return AVERROR_INVALIDDATA;
    }

    s->HEVClc->first_qp_group = !s->sh.dependent_slice_segment_flag;

    if (!s->ps.pps->cu_qp_delta_enabled_flag)
        s->HEVClc->qp_y = s->sh.slice_qp;

    s->slice_initialized = 1;
    s->HEVClc->tu.cu_qp_offset_cb = 0;
    s->HEVClc->tu.cu_qp_offset_cr = 0;

    return 0;
}

#define CTB(tab, x, y) ((tab)[(y) * s->ps.sps->ctb_width + (x)])

#define SET_SAO(elem, value)                            \
do {                                                    \
    if (!sao_merge_up_flag && !sao_merge_left_flag)     \
        sao->elem = value;                              \
    else if (sao_merge_left_flag)                       \
        sao->elem = CTB(s->sao, rx-1, ry).elem;         \
    else if (sao_merge_up_flag)                         \
        sao->elem = CTB(s->sao, rx, ry-1).elem;         \
    else                                                \
        sao->elem = 0;                                  \
} while (0)

static void hls_sao_param(HEVCContext *s, int rx, int ry)
{
    HEVCLocalContext *lc    = s->HEVClc;
    int sao_merge_left_flag = 0;
    int sao_merge_up_flag   = 0;
    SAOParams *sao          = &CTB(s->sao, rx, ry);
    int c_idx, i;

    if (s->sh.slice_sample_adaptive_offset_flag[0] ||
        s->sh.slice_sample_adaptive_offset_flag[1]) {
        if (rx > 0) {
            if (lc->ctb_left_flag)
                sao_merge_left_flag = ff_hevc_sao_merge_flag_decode(s);
        }
        if (ry > 0 && !sao_merge_left_flag) {
            if (lc->ctb_up_flag)
                sao_merge_up_flag = ff_hevc_sao_merge_flag_decode(s);
        }
    }

    for (c_idx = 0; c_idx < (s->ps.sps->chroma_format_idc ? 3 : 1); c_idx++) {
        int log2_sao_offset_scale = c_idx == 0 ? s->ps.pps->log2_sao_offset_scale_luma :
                                                 s->ps.pps->log2_sao_offset_scale_chroma;

        if (!s->sh.slice_sample_adaptive_offset_flag[c_idx]) {
            sao->type_idx[c_idx] = SAO_NOT_APPLIED;
            continue;
        }

        if (c_idx == 2) {
            sao->type_idx[2] = sao->type_idx[1];
            sao->eo_class[2] = sao->eo_class[1];
        } else {
            SET_SAO(type_idx[c_idx], ff_hevc_sao_type_idx_decode(s));
        }

        if (sao->type_idx[c_idx] == SAO_NOT_APPLIED)
            continue;

        for (i = 0; i < 4; i++)
            SET_SAO(offset_abs[c_idx][i], ff_hevc_sao_offset_abs_decode(s));

        if (sao->type_idx[c_idx] == SAO_BAND) {
            for (i = 0; i < 4; i++) {
                if (sao->offset_abs[c_idx][i]) {
                    SET_SAO(offset_sign[c_idx][i],
                            ff_hevc_sao_offset_sign_decode(s));
                } else {
                    sao->offset_sign[c_idx][i] = 0;
                }
            }
            SET_SAO(band_position[c_idx], ff_hevc_sao_band_position_decode(s));
        } else if (c_idx != 2) {
            SET_SAO(eo_class[c_idx], ff_hevc_sao_eo_class_decode(s));
        }

        // Inferred parameters
        sao->offset_val[c_idx][0] = 0;
        for (i = 0; i < 4; i++) {
            sao->offset_val[c_idx][i + 1] = sao->offset_abs[c_idx][i];
            if (sao->type_idx[c_idx] == SAO_EDGE) {
                if (i > 1)
                    sao->offset_val[c_idx][i + 1] = -sao->offset_val[c_idx][i + 1];
            } else if (sao->offset_sign[c_idx][i]) {
                sao->offset_val[c_idx][i + 1] = -sao->offset_val[c_idx][i + 1];
            }
            sao->offset_val[c_idx][i + 1] *= 1 << log2_sao_offset_scale;
        }
    }
}

#undef SET_SAO
#undef CTB

static int hls_cross_component_pred(HEVCContext *s, int idx) {
    HEVCLocalContext *lc    = s->HEVClc;
    int log2_res_scale_abs_plus1 = ff_hevc_log2_res_scale_abs(s, idx);

    if (log2_res_scale_abs_plus1 !=  0) {
        int res_scale_sign_flag = ff_hevc_res_scale_sign_flag(s, idx);
        lc->tu.res_scale_val = (1 << (log2_res_scale_abs_plus1 - 1)) *
                               (1 - 2 * res_scale_sign_flag);
    } else {
        lc->tu.res_scale_val = 0;
    }


    return 0;
}

#ifdef RPI
static inline HEVCPredCmd * rpi_new_intra_cmd(HEVCContext * const s)
{
    return s->jb0->intra.cmds + s->jb0->intra.n++;
}

static void rpi_intra_pred(HEVCContext *s, int log2_trafo_size, int x0, int y0, int c_idx)
{
    // U & V done on U call in the case of sliced frames
    if (av_rpi_is_sand_frame(s->frame) && c_idx > 1)
        return;

    if (s->enable_rpi) {
        HEVCLocalContext *lc = s->HEVClc;
        HEVCPredCmd *cmd = rpi_new_intra_cmd(s);
        cmd->type = RPI_PRED_INTRA;
        cmd->size = log2_trafo_size;
        cmd->na = (lc->na.cand_bottom_left<<4) + (lc->na.cand_left<<3) + (lc->na.cand_up_left<<2) + (lc->na.cand_up<<1) + lc->na.cand_up_right;
        cmd->c_idx = c_idx;
        cmd->i_pred.x = x0;
        cmd->i_pred.y = y0;
        cmd->i_pred.mode = c_idx ? lc->tu.intra_pred_mode_c :  lc->tu.intra_pred_mode;
    }
    else if (av_rpi_is_sand_frame(s->frame) && c_idx != 0) {
        s->hpc.intra_pred_c[log2_trafo_size - 2](s, x0, y0, c_idx);
    }
    else {
        s->hpc.intra_pred[log2_trafo_size - 2](s, x0, y0, c_idx);
    }

}
#endif

static int hls_transform_unit(HEVCContext *s, int x0, int y0,
                              int xBase, int yBase, int cb_xBase, int cb_yBase,
                              int log2_cb_size, int log2_trafo_size,
                              int blk_idx, int cbf_luma, int *cbf_cb, int *cbf_cr)
{
    HEVCLocalContext *lc = s->HEVClc;
    const int log2_trafo_size_c = log2_trafo_size - s->ps.sps->hshift[1];
    int i;

    if (lc->cu.pred_mode == MODE_INTRA) {
        int trafo_size = 1 << log2_trafo_size;
        ff_hevc_set_neighbour_available(s, x0, y0, trafo_size, trafo_size);
#ifdef RPI
        rpi_intra_pred(s, log2_trafo_size, x0, y0, 0);
#else
        s->hpc.intra_pred[log2_trafo_size - 2](s, x0, y0, 0);
#endif
    }

    if (cbf_luma || cbf_cb[0] || cbf_cr[0] ||
        (s->ps.sps->chroma_format_idc == 2 && (cbf_cb[1] || cbf_cr[1]))) {
        int scan_idx   = SCAN_DIAG;
        int scan_idx_c = SCAN_DIAG;
        int cbf_chroma = cbf_cb[0] || cbf_cr[0] ||
                         (s->ps.sps->chroma_format_idc == 2 &&
                         (cbf_cb[1] || cbf_cr[1]));

        if (s->ps.pps->cu_qp_delta_enabled_flag && !lc->tu.is_cu_qp_delta_coded) {
            lc->tu.cu_qp_delta = ff_hevc_cu_qp_delta_abs(s);
            if (lc->tu.cu_qp_delta != 0)
                if (ff_hevc_cu_qp_delta_sign_flag(s) == 1)
                    lc->tu.cu_qp_delta = -lc->tu.cu_qp_delta;
            lc->tu.is_cu_qp_delta_coded = 1;

            if (lc->tu.cu_qp_delta < -(26 + s->ps.sps->qp_bd_offset / 2) ||
                lc->tu.cu_qp_delta >  (25 + s->ps.sps->qp_bd_offset / 2)) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "The cu_qp_delta %d is outside the valid range "
                       "[%d, %d].\n",
                       lc->tu.cu_qp_delta,
                       -(26 + s->ps.sps->qp_bd_offset / 2),
                        (25 + s->ps.sps->qp_bd_offset / 2));
                return AVERROR_INVALIDDATA;
            }

            ff_hevc_set_qPy(s, cb_xBase, cb_yBase, log2_cb_size);
        }

        if (s->sh.cu_chroma_qp_offset_enabled_flag && cbf_chroma &&
            !lc->cu.cu_transquant_bypass_flag  &&  !lc->tu.is_cu_chroma_qp_offset_coded) {
            int cu_chroma_qp_offset_flag = ff_hevc_cu_chroma_qp_offset_flag(s);
            if (cu_chroma_qp_offset_flag) {
                int cu_chroma_qp_offset_idx  = 0;
                if (s->ps.pps->chroma_qp_offset_list_len_minus1 > 0) {
                    cu_chroma_qp_offset_idx = ff_hevc_cu_chroma_qp_offset_idx(s);
                    av_log(s->avctx, AV_LOG_ERROR,
                        "cu_chroma_qp_offset_idx not yet tested.\n");
                }
                lc->tu.cu_qp_offset_cb = s->ps.pps->cb_qp_offset_list[cu_chroma_qp_offset_idx];
                lc->tu.cu_qp_offset_cr = s->ps.pps->cr_qp_offset_list[cu_chroma_qp_offset_idx];
            } else {
                lc->tu.cu_qp_offset_cb = 0;
                lc->tu.cu_qp_offset_cr = 0;
            }
            lc->tu.is_cu_chroma_qp_offset_coded = 1;
        }

        if (lc->cu.pred_mode == MODE_INTRA && log2_trafo_size < 4) {
            if (lc->tu.intra_pred_mode >= 6 &&
                lc->tu.intra_pred_mode <= 14) {
                scan_idx = SCAN_VERT;
            } else if (lc->tu.intra_pred_mode >= 22 &&
                       lc->tu.intra_pred_mode <= 30) {
                scan_idx = SCAN_HORIZ;
            }

            if (lc->tu.intra_pred_mode_c >=  6 &&
                lc->tu.intra_pred_mode_c <= 14) {
                scan_idx_c = SCAN_VERT;
            } else if (lc->tu.intra_pred_mode_c >= 22 &&
                       lc->tu.intra_pred_mode_c <= 30) {
                scan_idx_c = SCAN_HORIZ;
            }
        }

        lc->tu.cross_pf = 0;

        if (cbf_luma)
            ff_hevc_hls_residual_coding(s, x0, y0, log2_trafo_size, scan_idx, 0);
        if (s->ps.sps->chroma_format_idc && (log2_trafo_size > 2 || s->ps.sps->chroma_format_idc == 3)) {
            int trafo_size_h = 1 << (log2_trafo_size_c + s->ps.sps->hshift[1]);
            int trafo_size_v = 1 << (log2_trafo_size_c + s->ps.sps->vshift[1]);
            lc->tu.cross_pf  = (s->ps.pps->cross_component_prediction_enabled_flag && cbf_luma &&
                                (lc->cu.pred_mode == MODE_INTER ||
                                 (lc->tu.chroma_mode_c ==  4)));

            if (lc->tu.cross_pf) {
                hls_cross_component_pred(s, 0);
            }
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, x0, y0 + (i << log2_trafo_size_c), trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (i << log2_trafo_size_c), 1);
#else
                    s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (i << log2_trafo_size_c), 1);
#endif
                }
                if (cbf_cb[i])
                    ff_hevc_hls_residual_coding(s, x0, y0 + (i << log2_trafo_size_c),
                                                log2_trafo_size_c, scan_idx_c, 1);
                else
                    if (lc->tu.cross_pf) {
                        ptrdiff_t stride = s->frame->linesize[1];
                        int hshift = s->ps.sps->hshift[1];
                        int vshift = s->ps.sps->vshift[1];
                        int16_t *coeffs_y = (int16_t*)lc->edge_emu_buffer;
                        int16_t *coeffs   = (int16_t*)lc->edge_emu_buffer2;
                        int size = 1 << log2_trafo_size_c;

                        uint8_t *dst = &s->frame->data[1][(y0 >> vshift) * stride +
                                                              ((x0 >> hshift) << s->ps.sps->pixel_shift)];
                        for (i = 0; i < (size * size); i++) {
                            coeffs[i] = ((lc->tu.res_scale_val * coeffs_y[i]) >> 3);
                        }
                        s->hevcdsp.add_residual[log2_trafo_size_c-2](dst, coeffs, stride);
                    }
            }

            if (lc->tu.cross_pf) {
                hls_cross_component_pred(s, 1);
            }
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, x0, y0 + (i << log2_trafo_size_c), trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (i << log2_trafo_size_c), 2);
#else
                    s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (i << log2_trafo_size_c), 2);
#endif
                }
                if (cbf_cr[i])
                    ff_hevc_hls_residual_coding(s, x0, y0 + (i << log2_trafo_size_c),
                                                log2_trafo_size_c, scan_idx_c, 2);
                else
                    if (lc->tu.cross_pf) {
                        ptrdiff_t stride = s->frame->linesize[2];
                        int hshift = s->ps.sps->hshift[2];
                        int vshift = s->ps.sps->vshift[2];
                        int16_t *coeffs_y = (int16_t*)lc->edge_emu_buffer;
                        int16_t *coeffs   = (int16_t*)lc->edge_emu_buffer2;
                        int size = 1 << log2_trafo_size_c;

                        uint8_t *dst = &s->frame->data[2][(y0 >> vshift) * stride +
                                                          ((x0 >> hshift) << s->ps.sps->pixel_shift)];
                        for (i = 0; i < (size * size); i++) {
                            coeffs[i] = ((lc->tu.res_scale_val * coeffs_y[i]) >> 3);
                        }
                        s->hevcdsp.add_residual[log2_trafo_size_c-2](dst, coeffs, stride);
                    }
            }
        } else if (s->ps.sps->chroma_format_idc && blk_idx == 3) {
            int trafo_size_h = 1 << (log2_trafo_size + 1);
            int trafo_size_v = 1 << (log2_trafo_size + s->ps.sps->vshift[1]);
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, xBase, yBase + (i << log2_trafo_size),
                                                    trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (i << log2_trafo_size), 1);
#else
                    s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (i << log2_trafo_size), 1);
#endif
                }
                if (cbf_cb[i])
                    ff_hevc_hls_residual_coding(s, xBase, yBase + (i << log2_trafo_size),
                                                log2_trafo_size, scan_idx_c, 1);
            }
            for (i = 0; i < (s->ps.sps->chroma_format_idc == 2 ? 2 : 1); i++) {
                if (lc->cu.pred_mode == MODE_INTRA) {
                    ff_hevc_set_neighbour_available(s, xBase, yBase + (i << log2_trafo_size),
                                                trafo_size_h, trafo_size_v);
#ifdef RPI
                    rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (i << log2_trafo_size), 2);
#else
                    s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (i << log2_trafo_size), 2);
#endif
                }
                if (cbf_cr[i])
                    ff_hevc_hls_residual_coding(s, xBase, yBase + (i << log2_trafo_size),
                                                log2_trafo_size, scan_idx_c, 2);
            }
        }
    } else if (s->ps.sps->chroma_format_idc && lc->cu.pred_mode == MODE_INTRA) {
        if (log2_trafo_size > 2 || s->ps.sps->chroma_format_idc == 3) {
            int trafo_size_h = 1 << (log2_trafo_size_c + s->ps.sps->hshift[1]);
            int trafo_size_v = 1 << (log2_trafo_size_c + s->ps.sps->vshift[1]);
            ff_hevc_set_neighbour_available(s, x0, y0, trafo_size_h, trafo_size_v);
#ifdef RPI
            rpi_intra_pred(s, log2_trafo_size_c, x0, y0, 1);
            rpi_intra_pred(s, log2_trafo_size_c, x0, y0, 2);
#else
            s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0, 1);
            s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0, 2);
#endif
            if (s->ps.sps->chroma_format_idc == 2) {
                ff_hevc_set_neighbour_available(s, x0, y0 + (1 << log2_trafo_size_c),
                                                trafo_size_h, trafo_size_v);
#ifdef RPI
                rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (1 << log2_trafo_size_c), 1);
                rpi_intra_pred(s, log2_trafo_size_c, x0, y0 + (1 << log2_trafo_size_c), 2);
#else
                s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (1 << log2_trafo_size_c), 1);
                s->hpc.intra_pred[log2_trafo_size_c - 2](s, x0, y0 + (1 << log2_trafo_size_c), 2);
#endif
            }
        } else if (blk_idx == 3) {
            int trafo_size_h = 1 << (log2_trafo_size + 1);
            int trafo_size_v = 1 << (log2_trafo_size + s->ps.sps->vshift[1]);
            ff_hevc_set_neighbour_available(s, xBase, yBase,
                                            trafo_size_h, trafo_size_v);
#ifdef RPI
            rpi_intra_pred(s, log2_trafo_size, xBase, yBase, 1);
            rpi_intra_pred(s, log2_trafo_size, xBase, yBase, 2);
#else
            s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase, 1);
            s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase, 2);
#endif
            if (s->ps.sps->chroma_format_idc == 2) {
                ff_hevc_set_neighbour_available(s, xBase, yBase + (1 << (log2_trafo_size)),
                                                trafo_size_h, trafo_size_v);
#ifdef RPI
                rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (1 << (log2_trafo_size)), 1);
                rpi_intra_pred(s, log2_trafo_size, xBase, yBase + (1 << (log2_trafo_size)), 2);
#else
                s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (1 << (log2_trafo_size)), 1);
                s->hpc.intra_pred[log2_trafo_size - 2](s, xBase, yBase + (1 << (log2_trafo_size)), 2);
#endif
            }
        }
    }

    return 0;
}

static void set_deblocking_bypass(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int cb_size          = 1 << log2_cb_size;
    int log2_min_pu_size = s->ps.sps->log2_min_pu_size;

    int min_pu_width     = s->ps.sps->min_pu_width;
    int x_end = FFMIN(x0 + cb_size, s->ps.sps->width);
    int y_end = FFMIN(y0 + cb_size, s->ps.sps->height);
    int i, j;

    for (j = (y0 >> log2_min_pu_size); j < (y_end >> log2_min_pu_size); j++)
        for (i = (x0 >> log2_min_pu_size); i < (x_end >> log2_min_pu_size); i++)
            s->is_pcm[i + j * min_pu_width] = 2;
}

static int hls_transform_tree(HEVCContext *s, int x0, int y0,
                              int xBase, int yBase, int cb_xBase, int cb_yBase,
                              int log2_cb_size, int log2_trafo_size,
                              int trafo_depth, int blk_idx,
                              const int *base_cbf_cb, const int *base_cbf_cr)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t split_transform_flag;
    int cbf_cb[2];
    int cbf_cr[2];
    int ret;

    cbf_cb[0] = base_cbf_cb[0];
    cbf_cb[1] = base_cbf_cb[1];
    cbf_cr[0] = base_cbf_cr[0];
    cbf_cr[1] = base_cbf_cr[1];

    if (lc->cu.intra_split_flag) {
        if (trafo_depth == 1) {
            lc->tu.intra_pred_mode   = lc->pu.intra_pred_mode[blk_idx];
            if (s->ps.sps->chroma_format_idc == 3) {
                lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[blk_idx];
                lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[blk_idx];
            } else {
                lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[0];
                lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[0];
            }
        }
    } else {
        lc->tu.intra_pred_mode   = lc->pu.intra_pred_mode[0];
        lc->tu.intra_pred_mode_c = lc->pu.intra_pred_mode_c[0];
        lc->tu.chroma_mode_c     = lc->pu.chroma_mode_c[0];
    }

    if (log2_trafo_size <= s->ps.sps->log2_max_trafo_size &&
        log2_trafo_size >  s->ps.sps->log2_min_tb_size    &&
        trafo_depth     < lc->cu.max_trafo_depth       &&
        !(lc->cu.intra_split_flag && trafo_depth == 0)) {
        split_transform_flag = ff_hevc_split_transform_flag_decode(s, log2_trafo_size);
    } else {
        int inter_split = s->ps.sps->max_transform_hierarchy_depth_inter == 0 &&
                          lc->cu.pred_mode == MODE_INTER &&
                          lc->cu.part_mode != PART_2Nx2N &&
                          trafo_depth == 0;

        split_transform_flag = log2_trafo_size > s->ps.sps->log2_max_trafo_size ||
                               (lc->cu.intra_split_flag && trafo_depth == 0) ||
                               inter_split;
    }

    if (s->ps.sps->chroma_format_idc && (log2_trafo_size > 2 || s->ps.sps->chroma_format_idc == 3)) {
        if (trafo_depth == 0 || cbf_cb[0]) {
            cbf_cb[0] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            if (s->ps.sps->chroma_format_idc == 2 && (!split_transform_flag || log2_trafo_size == 3)) {
                cbf_cb[1] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            }
        }

        if (trafo_depth == 0 || cbf_cr[0]) {
            cbf_cr[0] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            if (s->ps.sps->chroma_format_idc == 2 && (!split_transform_flag || log2_trafo_size == 3)) {
                cbf_cr[1] = ff_hevc_cbf_cb_cr_decode(s, trafo_depth);
            }
        }
    }

    if (split_transform_flag) {
        const int trafo_size_split = 1 << (log2_trafo_size - 1);
        const int x1 = x0 + trafo_size_split;
        const int y1 = y0 + trafo_size_split;

#define SUBDIVIDE(x, y, idx)                                                    \
do {                                                                            \
    ret = hls_transform_tree(s, x, y, x0, y0, cb_xBase, cb_yBase, log2_cb_size, \
                             log2_trafo_size - 1, trafo_depth + 1, idx,         \
                             cbf_cb, cbf_cr);                                   \
    if (ret < 0)                                                                \
        return ret;                                                             \
} while (0)

        SUBDIVIDE(x0, y0, 0);
        SUBDIVIDE(x1, y0, 1);
        SUBDIVIDE(x0, y1, 2);
        SUBDIVIDE(x1, y1, 3);

#undef SUBDIVIDE
    } else {
        int min_tu_size      = 1 << s->ps.sps->log2_min_tb_size;
        int log2_min_tu_size = s->ps.sps->log2_min_tb_size;
        int min_tu_width     = s->ps.sps->min_tb_width;
        int cbf_luma         = 1;

        if (lc->cu.pred_mode == MODE_INTRA || trafo_depth != 0 ||
            cbf_cb[0] || cbf_cr[0] ||
            (s->ps.sps->chroma_format_idc == 2 && (cbf_cb[1] || cbf_cr[1]))) {
            cbf_luma = ff_hevc_cbf_luma_decode(s, trafo_depth);
        }

        ret = hls_transform_unit(s, x0, y0, xBase, yBase, cb_xBase, cb_yBase,
                                 log2_cb_size, log2_trafo_size,
                                 blk_idx, cbf_luma, cbf_cb, cbf_cr);
        if (ret < 0)
            return ret;
        // TODO: store cbf_luma somewhere else
        if (cbf_luma) {
            int i, j;
            for (i = 0; i < (1 << log2_trafo_size); i += min_tu_size)
                for (j = 0; j < (1 << log2_trafo_size); j += min_tu_size) {
                    int x_tu = (x0 + j) >> log2_min_tu_size;
                    int y_tu = (y0 + i) >> log2_min_tu_size;
                    s->cbf_luma[y_tu * min_tu_width + x_tu] = 1;
                }
        }
        if (!s->sh.disable_deblocking_filter_flag) {
            ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_trafo_size);
            if (s->ps.pps->transquant_bypass_enable_flag &&
                lc->cu.cu_transquant_bypass_flag)
                set_deblocking_bypass(s, x0, y0, log2_trafo_size);
        }
    }
    return 0;
}


static int pcm_extract(HEVCContext * const s, const uint8_t * pcm, const int length, const int x0, const int y0, const int cb_size)
{
    GetBitContext gb;
    int ret;

    ret = init_get_bits(&gb, pcm, length);
    if (ret < 0)
        return ret;

#if RPI_HEVC_SAND
    if (av_rpi_is_sand_frame(s->frame)) {
        s->hevcdsp.put_pcm(av_rpi_sand_frame_pos_y(s->frame, x0, y0),
                           s->frame->linesize[0],
                           cb_size, cb_size, &gb, s->ps.sps->pcm.bit_depth);

        s->hevcdsp.put_pcm_c(av_rpi_sand_frame_pos_c(s->frame, x0 >> s->ps.sps->hshift[1], y0 >> s->ps.sps->vshift[1]),
                           s->frame->linesize[1],
                           cb_size >> s->ps.sps->hshift[1],
                           cb_size >> s->ps.sps->vshift[1],
                           &gb, s->ps.sps->pcm.bit_depth_chroma);
    }
    else
#endif
    {
        const int stride0   = s->frame->linesize[0];
        uint8_t * const dst0 = &s->frame->data[0][y0 * stride0 + (x0 << s->ps.sps->pixel_shift)];
        const int   stride1 = s->frame->linesize[1];
        uint8_t * const dst1 = &s->frame->data[1][(y0 >> s->ps.sps->vshift[1]) * stride1 + ((x0 >> s->ps.sps->hshift[1]) << s->ps.sps->pixel_shift)];
        const int   stride2 = s->frame->linesize[2];
        uint8_t * const dst2 = &s->frame->data[2][(y0 >> s->ps.sps->vshift[2]) * stride2 + ((x0 >> s->ps.sps->hshift[2]) << s->ps.sps->pixel_shift)];

        s->hevcdsp.put_pcm(dst0, stride0, cb_size, cb_size, &gb, s->ps.sps->pcm.bit_depth);
        if (s->ps.sps->chroma_format_idc) {
            s->hevcdsp.put_pcm(dst1, stride1,
                               cb_size >> s->ps.sps->hshift[1],
                               cb_size >> s->ps.sps->vshift[1],
                               &gb, s->ps.sps->pcm.bit_depth_chroma);
            s->hevcdsp.put_pcm(dst2, stride2,
                               cb_size >> s->ps.sps->hshift[2],
                               cb_size >> s->ps.sps->vshift[2],
                               &gb, s->ps.sps->pcm.bit_depth_chroma);
        }

    }
    return 0;
}

#ifdef RPI
int16_t * rpi_alloc_coeff_buf(HEVCContext * const s, const int buf_no, const int n)
{
    HEVCRpiCoeffEnv *const cfe = s->jb0->coeffs.s + buf_no;
    int16_t * const coeffs = (buf_no != 3) ? cfe->buf + cfe->n : cfe->buf - (cfe->n + n);
    cfe->n += n;
    return coeffs;
}
#endif

// x * 2^(y*2)
static inline unsigned int xyexp2(const unsigned int x, const unsigned int y)
{
    return x << (y * 2);
}

static int hls_pcm_sample(HEVCContext * const s, const int x0, const int y0, unsigned int log2_cb_size)
{
    // Length in bits
    const unsigned int length = xyexp2(s->ps.sps->pcm.bit_depth, log2_cb_size) +
        xyexp2(s->ps.sps->pcm.bit_depth_chroma, log2_cb_size - s->ps.sps->vshift[1]) +
        xyexp2(s->ps.sps->pcm.bit_depth_chroma, log2_cb_size - s->ps.sps->vshift[2]);

    const uint8_t * const pcm = skip_bytes(&s->HEVClc->cc, (length + 7) >> 3);

    if (!s->sh.disable_deblocking_filter_flag)
        ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);

#ifdef RPI
    if (s->enable_rpi) {
        // Copy coeffs
        const int blen = (length + 7) >> 3;
        // Round allocated bytes up to nearest 32 to avoid alignment confusion
        // Allocation is in int16_t s
        // As we are only using 1 byte per sample and the coeff buffer allows 2 per
        // sample this rounding doesn't affect the total size we need to allocate for
        // the coeff buffer
        int16_t * const coeffs = rpi_alloc_coeff_buf(s, 0, ((blen + 31) & ~31) >> 1);
        memcpy(coeffs, pcm, blen);

        // Our coeff stash assumes that any partially allocated 64byte lump
        // is zeroed so make that true.
        {
            uint8_t * const eopcm = (uint8_t *)coeffs + blen;
            if ((-(intptr_t)eopcm & 63) != 0)
                memset(eopcm, 0, -(intptr_t)eopcm & 63);
        }

        // Add command
        {
            HEVCPredCmd *const cmd = rpi_new_intra_cmd(s);
            cmd->type = RPI_PRED_I_PCM;
            cmd->size = log2_cb_size;
            cmd->i_pcm.src = coeffs;
            cmd->i_pcm.x = x0;
            cmd->i_pcm.y = y0;
            cmd->i_pcm.src_len = length;
        }
        return 0;
    }
#endif

    return pcm_extract(s, pcm, length, x0, y0, 1 << log2_cb_size);
}

/**
 * 8.5.3.2.2.1 Luma sample unidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param luma_weight weighting factor applied to the luma prediction
 * @param luma_offset additive offset applied to the luma prediction value
 */

static void luma_mc_uni(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                        AVFrame *ref, const Mv *mv, int x_off, int y_off,
                        int block_w, int block_h, int luma_weight, int luma_offset)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t *src         = ref->data[0];
    ptrdiff_t srcstride  = ref->linesize[0];
    int pic_width        = s->ps.sps->width;
    int pic_height       = s->ps.sps->height;
    int mx               = mv->x & 3;
    int my               = mv->y & 3;
    int weight_flag      = (s->sh.slice_type == HEVC_SLICE_P && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == HEVC_SLICE_B && s->ps.pps->weighted_bipred_flag);
    int idx              = ff_hevc_pel_weight[block_w];

#ifdef DISABLE_MC
    return;
#endif

    x_off += mv->x >> 2;
    y_off += mv->y >> 2;
    src   += y_off * srcstride + (x_off * (1 << s->ps.sps->pixel_shift));

    if (x_off < QPEL_EXTRA_BEFORE || y_off < QPEL_EXTRA_AFTER ||
        x_off >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const ptrdiff_t edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * srcstride       + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src - offset,
                                 edge_emu_stride, srcstride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off - QPEL_EXTRA_BEFORE, y_off - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src = lc->edge_emu_buffer + buf_offset;
        srcstride = edge_emu_stride;
    }

    if (!weight_flag)
        s->hevcdsp.put_hevc_qpel_uni[idx][!!my][!!mx](dst, dststride, src, srcstride,
                                                      block_h, mx, my, block_w);
    else
        s->hevcdsp.put_hevc_qpel_uni_w[idx][!!my][!!mx](dst, dststride, src, srcstride,
                                                        block_h, s->sh.luma_log2_weight_denom,
                                                        luma_weight, luma_offset, mx, my, block_w);
}

/**
 * 8.5.3.2.2.1 Luma sample bidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref0 reference picture0 buffer at origin (0, 0)
 * @param mv0 motion vector0 (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param ref1 reference picture1 buffer at origin (0, 0)
 * @param mv1 motion vector1 (relative to block position) to get pixel data from
 * @param current_mv current motion vector structure
 */
static void luma_mc_bi(HEVCContext *s, uint8_t *dst, ptrdiff_t dststride,
                       AVFrame *ref0, const Mv *mv0, int x_off, int y_off,
                       int block_w, int block_h, AVFrame *ref1, const Mv *mv1, struct MvField *current_mv)
{
    HEVCLocalContext *lc = s->HEVClc;
    ptrdiff_t src0stride  = ref0->linesize[0];
    ptrdiff_t src1stride  = ref1->linesize[0];
    int pic_width        = s->ps.sps->width;
    int pic_height       = s->ps.sps->height;
    int mx0              = mv0->x & 3;
    int my0              = mv0->y & 3;
    int mx1              = mv1->x & 3;
    int my1              = mv1->y & 3;
    int weight_flag      = (s->sh.slice_type == HEVC_SLICE_P && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == HEVC_SLICE_B && s->ps.pps->weighted_bipred_flag);
    int x_off0           = x_off + (mv0->x >> 2);
    int y_off0           = y_off + (mv0->y >> 2);
    int x_off1           = x_off + (mv1->x >> 2);
    int y_off1           = y_off + (mv1->y >> 2);
    int idx              = ff_hevc_pel_weight[block_w];

    uint8_t *src0  = ref0->data[0] + y_off0 * src0stride + (int)((unsigned)x_off0 << s->ps.sps->pixel_shift);
    uint8_t *src1  = ref1->data[0] + y_off1 * src1stride + (int)((unsigned)x_off1 << s->ps.sps->pixel_shift);

#ifdef DISABLE_MC
    return;
#endif

    if (x_off0 < QPEL_EXTRA_BEFORE || y_off0 < QPEL_EXTRA_AFTER ||
        x_off0 >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off0 >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const ptrdiff_t edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * src0stride       + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src0 - offset,
                                 edge_emu_stride, src0stride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off0 - QPEL_EXTRA_BEFORE, y_off0 - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src0 = lc->edge_emu_buffer + buf_offset;
        src0stride = edge_emu_stride;
    }

    if (x_off1 < QPEL_EXTRA_BEFORE || y_off1 < QPEL_EXTRA_AFTER ||
        x_off1 >= pic_width - block_w - QPEL_EXTRA_AFTER ||
        y_off1 >= pic_height - block_h - QPEL_EXTRA_AFTER) {
        const ptrdiff_t edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset     = QPEL_EXTRA_BEFORE * src1stride       + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);
        int buf_offset = QPEL_EXTRA_BEFORE * edge_emu_stride + (QPEL_EXTRA_BEFORE << s->ps.sps->pixel_shift);

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer2, src1 - offset,
                                 edge_emu_stride, src1stride,
                                 block_w + QPEL_EXTRA,
                                 block_h + QPEL_EXTRA,
                                 x_off1 - QPEL_EXTRA_BEFORE, y_off1 - QPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);
        src1 = lc->edge_emu_buffer2 + buf_offset;
        src1stride = edge_emu_stride;
    }

    s->hevcdsp.put_hevc_qpel[idx][!!my0][!!mx0](lc->tmp, src0, src0stride,
                                                block_h, mx0, my0, block_w);
    if (!weight_flag)
        s->hevcdsp.put_hevc_qpel_bi[idx][!!my1][!!mx1](dst, dststride, src1, src1stride, lc->tmp,
                                                       block_h, mx1, my1, block_w);
    else
        s->hevcdsp.put_hevc_qpel_bi_w[idx][!!my1][!!mx1](dst, dststride, src1, src1stride, lc->tmp,
                                                         block_h, s->sh.luma_log2_weight_denom,
                                                         s->sh.luma_weight_l0[current_mv->ref_idx[0]],
                                                         s->sh.luma_weight_l1[current_mv->ref_idx[1]],
                                                         s->sh.luma_offset_l0[current_mv->ref_idx[0]],
                                                         s->sh.luma_offset_l1[current_mv->ref_idx[1]],
                                                         mx1, my1, block_w);

}

/**
 * 8.5.3.2.2.2 Chroma sample uniprediction interpolation process
 *
 * @param s HEVC decoding context
 * @param dst1 target buffer for block data at block position (U plane)
 * @param dst2 target buffer for block data at block position (V plane)
 * @param dststride stride of the dst1 and dst2 buffers
 * @param ref reference picture buffer at origin (0, 0)
 * @param mv motion vector (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param chroma_weight weighting factor applied to the chroma prediction
 * @param chroma_offset additive offset applied to the chroma prediction value
 */

static void chroma_mc_uni(HEVCContext *s, uint8_t *dst0,
                          ptrdiff_t dststride, uint8_t *src0, ptrdiff_t srcstride, int reflist,
                          int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int chroma_weight, int chroma_offset)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pic_width        = s->ps.sps->width >> s->ps.sps->hshift[1];
    int pic_height       = s->ps.sps->height >> s->ps.sps->vshift[1];
    const Mv *mv         = &current_mv->mv[reflist];
    int weight_flag      = (s->sh.slice_type == HEVC_SLICE_P && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == HEVC_SLICE_B && s->ps.pps->weighted_bipred_flag);
    int idx              = ff_hevc_pel_weight[block_w];
    int hshift           = s->ps.sps->hshift[1];
    int vshift           = s->ps.sps->vshift[1];
    intptr_t mx          = av_mod_uintp2(mv->x, 2 + hshift);
    intptr_t my          = av_mod_uintp2(mv->y, 2 + vshift);
    intptr_t _mx         = mx << (1 - hshift);
    intptr_t _my         = my << (1 - vshift);

#ifdef DISABLE_MC
    return;
#endif

    x_off += mv->x >> (2 + hshift);
    y_off += mv->y >> (2 + vshift);
    src0  += y_off * srcstride + (x_off * (1 << s->ps.sps->pixel_shift));

    if (x_off < EPEL_EXTRA_BEFORE || y_off < EPEL_EXTRA_AFTER ||
        x_off >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset0 = EPEL_EXTRA_BEFORE * (srcstride + (1 << s->ps.sps->pixel_shift));
        int buf_offset0 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->ps.sps->pixel_shift));
        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src0 - offset0,
                                 edge_emu_stride, srcstride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off - EPEL_EXTRA_BEFORE,
                                 y_off - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src0 = lc->edge_emu_buffer + buf_offset0;
        srcstride = edge_emu_stride;
    }
    if (!weight_flag)
        s->hevcdsp.put_hevc_epel_uni[idx][!!my][!!mx](dst0, dststride, src0, srcstride,
                                                  block_h, _mx, _my, block_w);
    else
        s->hevcdsp.put_hevc_epel_uni_w[idx][!!my][!!mx](dst0, dststride, src0, srcstride,
                                                        block_h, s->sh.chroma_log2_weight_denom,
                                                        chroma_weight, chroma_offset, _mx, _my, block_w);
}

/**
 * 8.5.3.2.2.2 Chroma sample bidirectional interpolation process
 *
 * @param s HEVC decoding context
 * @param dst target buffer for block data at block position
 * @param dststride stride of the dst buffer
 * @param ref0 reference picture0 buffer at origin (0, 0)
 * @param mv0 motion vector0 (relative to block position) to get pixel data from
 * @param x_off horizontal position of block from origin (0, 0)
 * @param y_off vertical position of block from origin (0, 0)
 * @param block_w width of block
 * @param block_h height of block
 * @param ref1 reference picture1 buffer at origin (0, 0)
 * @param mv1 motion vector1 (relative to block position) to get pixel data from
 * @param current_mv current motion vector structure
 * @param cidx chroma component(cb, cr)
 */
static void chroma_mc_bi(HEVCContext *s, uint8_t *dst0, ptrdiff_t dststride, AVFrame *ref0, AVFrame *ref1,
                         int x_off, int y_off, int block_w, int block_h, struct MvField *current_mv, int cidx)
{
    HEVCLocalContext *lc = s->HEVClc;
    uint8_t *src1        = ref0->data[cidx+1];
    uint8_t *src2        = ref1->data[cidx+1];
    ptrdiff_t src1stride = ref0->linesize[cidx+1];
    ptrdiff_t src2stride = ref1->linesize[cidx+1];
    int weight_flag      = (s->sh.slice_type == HEVC_SLICE_P && s->ps.pps->weighted_pred_flag) ||
                           (s->sh.slice_type == HEVC_SLICE_B && s->ps.pps->weighted_bipred_flag);
    int pic_width        = s->ps.sps->width >> s->ps.sps->hshift[1];
    int pic_height       = s->ps.sps->height >> s->ps.sps->vshift[1];
    Mv *mv0              = &current_mv->mv[0];
    Mv *mv1              = &current_mv->mv[1];
    int hshift = s->ps.sps->hshift[1];
    int vshift = s->ps.sps->vshift[1];

#ifdef DISABLE_MC
    return;
#endif

    intptr_t mx0 = av_mod_uintp2(mv0->x, 2 + hshift);
    intptr_t my0 = av_mod_uintp2(mv0->y, 2 + vshift);
    intptr_t mx1 = av_mod_uintp2(mv1->x, 2 + hshift);
    intptr_t my1 = av_mod_uintp2(mv1->y, 2 + vshift);
    intptr_t _mx0 = mx0 << (1 - hshift);
    intptr_t _my0 = my0 << (1 - vshift);
    intptr_t _mx1 = mx1 << (1 - hshift);
    intptr_t _my1 = my1 << (1 - vshift);

    int x_off0 = x_off + (mv0->x >> (2 + hshift));
    int y_off0 = y_off + (mv0->y >> (2 + vshift));
    int x_off1 = x_off + (mv1->x >> (2 + hshift));
    int y_off1 = y_off + (mv1->y >> (2 + vshift));
    int idx = ff_hevc_pel_weight[block_w];
    src1  += y_off0 * src1stride + (int)((unsigned)x_off0 << s->ps.sps->pixel_shift);
    src2  += y_off1 * src2stride + (int)((unsigned)x_off1 << s->ps.sps->pixel_shift);

    if (x_off0 < EPEL_EXTRA_BEFORE || y_off0 < EPEL_EXTRA_AFTER ||
        x_off0 >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off0 >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset1 = EPEL_EXTRA_BEFORE * (src1stride + (1 << s->ps.sps->pixel_shift));
        int buf_offset1 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->ps.sps->pixel_shift));

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer, src1 - offset1,
                                 edge_emu_stride, src1stride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off0 - EPEL_EXTRA_BEFORE,
                                 y_off0 - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src1 = lc->edge_emu_buffer + buf_offset1;
        src1stride = edge_emu_stride;
    }

    if (x_off1 < EPEL_EXTRA_BEFORE || y_off1 < EPEL_EXTRA_AFTER ||
        x_off1 >= pic_width - block_w - EPEL_EXTRA_AFTER ||
        y_off1 >= pic_height - block_h - EPEL_EXTRA_AFTER) {
        const int edge_emu_stride = EDGE_EMU_BUFFER_STRIDE << s->ps.sps->pixel_shift;
        int offset1 = EPEL_EXTRA_BEFORE * (src2stride + (1 << s->ps.sps->pixel_shift));
        int buf_offset1 = EPEL_EXTRA_BEFORE *
                          (edge_emu_stride + (1 << s->ps.sps->pixel_shift));

        s->vdsp.emulated_edge_mc(lc->edge_emu_buffer2, src2 - offset1,
                                 edge_emu_stride, src2stride,
                                 block_w + EPEL_EXTRA, block_h + EPEL_EXTRA,
                                 x_off1 - EPEL_EXTRA_BEFORE,
                                 y_off1 - EPEL_EXTRA_BEFORE,
                                 pic_width, pic_height);

        src2 = lc->edge_emu_buffer2 + buf_offset1;
        src2stride = edge_emu_stride;
    }

    s->hevcdsp.put_hevc_epel[idx][!!my0][!!mx0](lc->tmp, src1, src1stride,
                                                block_h, _mx0, _my0, block_w);
    if (!weight_flag)
        s->hevcdsp.put_hevc_epel_bi[idx][!!my1][!!mx1](dst0, s->frame->linesize[cidx+1],
                                                       src2, src2stride, lc->tmp,
                                                       block_h, _mx1, _my1, block_w);
    else
        s->hevcdsp.put_hevc_epel_bi_w[idx][!!my1][!!mx1](dst0, s->frame->linesize[cidx+1],
                                                         src2, src2stride, lc->tmp,
                                                         block_h,
                                                         s->sh.chroma_log2_weight_denom,
                                                         s->sh.chroma_weight_l0[current_mv->ref_idx[0]][cidx],
                                                         s->sh.chroma_weight_l1[current_mv->ref_idx[1]][cidx],
                                                         s->sh.chroma_offset_l0[current_mv->ref_idx[0]][cidx],
                                                         s->sh.chroma_offset_l1[current_mv->ref_idx[1]][cidx],
                                                         _mx1, _my1, block_w);
}

static void hevc_await_progress(HEVCContext *s, HEVCFrame *ref,
                                const Mv *mv, int y0, int height)
{
    int y = FFMAX(0, (mv->y >> 2) + y0 + height + 9);

    if (s->threads_type == FF_THREAD_FRAME )
        ff_thread_await_progress(&ref->tf, y, 0);
}

static void hevc_luma_mv_mvp_mode(HEVCContext *s, int x0, int y0, int nPbW,
                                  int nPbH, int log2_cb_size, int part_idx,
                                  int merge_idx, MvField *mv)
{
    HEVCLocalContext *lc = s->HEVClc;
    enum InterPredIdc inter_pred_idc = PRED_L0;
    int mvp_flag;

    ff_hevc_set_neighbour_available(s, x0, y0, nPbW, nPbH);
    mv->pred_flag = 0;
    if (s->sh.slice_type == HEVC_SLICE_B)
        inter_pred_idc = ff_hevc_inter_pred_idc_decode(s, nPbW, nPbH);

    if (inter_pred_idc != PRED_L1) {
        if (s->sh.nb_refs[L0])
            mv->ref_idx[0]= ff_hevc_ref_idx_lx_decode(s, s->sh.nb_refs[L0]);

        mv->pred_flag = PF_L0;
        ff_hevc_hls_mvd_coding(s, x0, y0, 0);
        mvp_flag = ff_hevc_mvp_lx_flag_decode(s);
        ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                 part_idx, merge_idx, mv, mvp_flag, 0);
        mv->mv[0].x += lc->pu.mvd.x;
        mv->mv[0].y += lc->pu.mvd.y;
    }

    if (inter_pred_idc != PRED_L0) {
        if (s->sh.nb_refs[L1])
            mv->ref_idx[1]= ff_hevc_ref_idx_lx_decode(s, s->sh.nb_refs[L1]);

        if (s->sh.mvd_l1_zero_flag == 1 && inter_pred_idc == PRED_BI) {
            AV_ZERO32(&lc->pu.mvd);
        } else {
            ff_hevc_hls_mvd_coding(s, x0, y0, 1);
        }

        mv->pred_flag += PF_L1;
        mvp_flag = ff_hevc_mvp_lx_flag_decode(s);
        ff_hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                 part_idx, merge_idx, mv, mvp_flag, 1);
        mv->mv[1].x += lc->pu.mvd.x;
        mv->mv[1].y += lc->pu.mvd.y;
    }
}


#if RPI_INTER

static HEVCRpiInterPredQ *
rpi_nxt_pred(HEVCRpiInterPredEnv * const ipe, const unsigned int load_val, const uint32_t fn)
{
    HEVCRpiInterPredQ * yp = ipe->q + ipe->curr;
    HEVCRpiInterPredQ * ypt = yp + 1;
    for (unsigned int i = 1; i != ipe->n_grp; ++i, ++ypt) {
        if (ypt->load < yp->load)
            yp = ypt;
    }

    yp->load += load_val;
    ipe->used_grp = 1;
    ((uint32_t *)yp->qpu_mc_curr)[-1] = fn;  // Link is always last el of previous cmd

    return yp;
}


static void rpi_inter_pred_sync(HEVCRpiInterPredEnv * const ipe)
{
    for (unsigned int i = 0; i != ipe->n; ++i) {
        HEVCRpiInterPredQ * const q = ipe->q + i;
        ((uint32_t *)q->qpu_mc_curr)[-1] = q->code_sync;
        q->qpu_mc_curr = (qpu_mc_pred_cmd_t *)((uint32_t *)q->qpu_mc_curr + 1);
        q->load = 0;
    }
}

// Returns 0 on success, -1 if Q is dangerously full
static int rpi_inter_pred_next_ctu(HEVCRpiInterPredEnv * const ipe)
{
    if (!ipe->used_grp)
        return 0;

    if ((ipe->curr += ipe->n_grp) >= ipe->n)
    {
        ipe->curr = 0;
        rpi_inter_pred_sync(ipe);
    }
    ipe->used = 1;
    ipe->used_grp = 0;

    for (unsigned int i = 0; i != ipe->n_grp; ++i) {
        HEVCRpiInterPredQ * const q = ipe->q + i + ipe->curr;
        if ((char *)q->qpu_mc_curr - (char *)q->qpu_mc_base > ipe->max_fill) {
            return -1;
        }
    }
    return 0;
}

static void rpi_inter_pred_reset(HEVCRpiInterPredEnv * const ipe)
{
    unsigned int i;
    ipe->curr = 0;
    ipe->used = 0;
    ipe->used_grp = 0;
    for (i = 0; i != ipe->n; ++i) {
        HEVCRpiInterPredQ * const q = ipe->q + i;
        q->qpu_mc_curr = q->qpu_mc_base;
        q->load = 0;
        q->last_l0 = NULL;
        q->last_l1 = NULL;
    }
}

static void rpi_inter_pred_alloc(HEVCRpiInterPredEnv * const ipe,
                                 const unsigned int n_max, const unsigned int n_grp,
                                 const unsigned int total_size, const unsigned int min_gap)
{
    memset(ipe, 0, sizeof(*ipe));
    av_assert0((ipe->q = av_mallocz(n_max * sizeof(*ipe->q))) != NULL);
    ipe->n_grp = n_grp;
    ipe->min_gap = min_gap;

#if RPI_CACHE_UNIF_MVS
    gpu_malloc_cached(total_size, &ipe->gptr);
#else
    gpu_malloc_uncached(total_size, &ipe->gptr);
#endif
}


#if RPI_QPU_EMU_Y
#define get_mc_address_y(f) ((f)->data[0])
#else
#define get_mc_address_y(f) get_vc_address_y(f)
#endif
#if RPI_QPU_EMU_C
#define get_mc_address_u(f) ((f)->data[1])
#else
#define get_mc_address_u(f) get_vc_address_u(f)
#endif

static inline int offset_depth_adj(const HEVCContext *const s, const int wt)
{
    return s->ps.sps->high_precision_offsets_enabled_flag ? wt :
           wt << (s->ps.sps->bit_depth - 8);
}

static void
rpi_pred_y(HEVCContext *const s, const int x0, const int y0,
           const int nPbW, const int nPbH,
           const Mv *const mv,
           const int weight_mul,
           const int weight_offset,
           AVFrame *const src_frame)
{
    const unsigned int y_off = av_rpi_sand_frame_off_y(s->frame, x0, y0);
    const unsigned int mx          = mv->x & 3;
    const unsigned int my          = mv->y & 3;
    const unsigned int my_mx       = (my << 8) | mx;
    const uint32_t     my2_mx2_my_mx = (my_mx << 16) | my_mx;
    const qpu_mc_src_addr_t src_vc_address_y = get_mc_address_y(src_frame);
    qpu_mc_dst_addr_t dst_addr = get_mc_address_y(s->frame) + y_off;
    const uint32_t wo = PACK2(offset_depth_adj(s, weight_offset) * 2 + 1, weight_mul);
    HEVCRpiInterPredEnv * const ipe = &s->jb0->luma_ip;
    const unsigned int xshl = av_rpi_sand_frame_xshl(s->frame);

    if (my_mx == 0)
    {
        const int x1 = x0 + (mv->x >> 2);
        const int y1 = y0 + (mv->y >> 2);
        const int bh = nPbH;

        for (int start_x = 0; start_x < nPbW; start_x += 16)
        {
            const int bw = FFMIN(nPbW - start_x, 16);
            HEVCRpiInterPredQ *const yp = rpi_nxt_pred(ipe, bh, s->qpu.y_p00);
            qpu_mc_src_t *const src1 = yp->last_l0;
            qpu_mc_pred_y_p00_t *const cmd_y = &yp->qpu_mc_curr->y.p00;

#if RPI_TSTATS
            {
                HEVCRpiStats *const ts = &s->tstats;
                ++ts->y_pred1_x0y0;

                if (nPbW > 8)
                    ++ts->y_pred1_wgt8;
                else
                    ++ts->y_pred1_wle8;

                if (nPbH > 16)
                    ++ts->y_pred1_hgt16;
                else
                    ++ts->y_pred1_hle16;
            }
#endif

            src1->x = x1 + start_x;
            src1->y = y1;
            src1->base = src_vc_address_y;
            cmd_y->w = bw;
            cmd_y->h = bh;
            cmd_y->wo1 = wo;
            cmd_y->dst_addr =  dst_addr + (start_x << xshl);
            yp->last_l0 = &cmd_y->next_src1;
            *(qpu_mc_pred_y_p00_t **)&yp->qpu_mc_curr = cmd_y + 1;
        }
    }
    else
    {
        const int x1_m3 = x0 + (mv->x >> 2) - 3;
        const int y1_m3 = y0 + (mv->y >> 2) - 3;
        const unsigned int bh = nPbH;
        int start_x = 0;

#if 1
        // As Y-pred operates on two independant 8-wide src blocks we can merge
        // this pred with the previous one if it the previous one is 8 pel wide,
        // the same height as the current block, immediately to the left of our
        // current dest block and mono-pred.

        qpu_mc_pred_y_p_t *const last_y8_p = s->last_y8_p;
        if (last_y8_p != NULL && last_y8_p->h == bh && last_y8_p->dst_addr + 8 == dst_addr)
        {
            const int bw = FFMIN(nPbW, 8);
            qpu_mc_src_t *const last_y8_src2 = s->last_y8_l1;

            last_y8_src2->x = x1_m3;
            last_y8_src2->y = y1_m3;
            last_y8_src2->base = src_vc_address_y;
            last_y8_p->w += bw;
            last_y8_p->mymx21 = PACK2(my2_mx2_my_mx, last_y8_p->mymx21);
            last_y8_p->wo2 = wo;

            s->last_y8_p = NULL;
            s->last_y8_l1 = NULL;
            start_x = bw;
#if RPI_TSTATS
            ++s->tstats.y_pred1_y8_merge;
#endif
        }
#endif

        for (; start_x < nPbW; start_x += 16)
        {
            const int bw = FFMIN(nPbW - start_x, 16);
            HEVCRpiInterPredQ *const yp = rpi_nxt_pred(ipe, bh + 7, s->qpu.y_pxx);
            qpu_mc_src_t *const src1 = yp->last_l0;
            qpu_mc_src_t *const src2 = yp->last_l1;
            qpu_mc_pred_y_p_t *const cmd_y = &yp->qpu_mc_curr->y.p;
#if RPI_TSTATS
            {
                HEVCRpiStats *const ts = &s->tstats;
                if (mx == 0 && my == 0)
                    ++ts->y_pred1_x0y0;
                else if (mx == 0)
                    ++ts->y_pred1_x0;
                else if (my == 0)
                    ++ts->y_pred1_y0;
                else
                    ++ts->y_pred1_xy;

                if (nPbW > 8)
                    ++ts->y_pred1_wgt8;
                else
                    ++ts->y_pred1_wle8;

                if (nPbH > 16)
                    ++ts->y_pred1_hgt16;
                else
                    ++ts->y_pred1_hle16;
            }
#endif
            src1->x = x1_m3 + start_x;
            src1->y = y1_m3;
            src1->base = src_vc_address_y;
            if (bw <= 8)
            {
                src2->x = MC_DUMMY_X;
                src2->y = MC_DUMMY_Y;
#if RPI_QPU_EMU_Y
                src2->base = s->qpu_dummy_frame_emu;
#else
                src2->base = s->qpu_dummy_frame_qpu;
#endif
            }
            else
            {
                src2->x = x1_m3 + start_x + 8;
                src2->y = y1_m3;
                src2->base = src_vc_address_y;
            }
            cmd_y->w = bw;
            cmd_y->h = bh;
            cmd_y->mymx21 = my2_mx2_my_mx;
            cmd_y->wo1 = wo;
            cmd_y->wo2 = wo;
            cmd_y->dst_addr =  dst_addr + (start_x << xshl);
            yp->last_l0 = &cmd_y->next_src1;
            yp->last_l1 = &cmd_y->next_src2;
            *(qpu_mc_pred_y_p_t **)&yp->qpu_mc_curr = cmd_y + 1;

            if (bw == 8) {
                s->last_y8_l1 = src2;
                s->last_y8_p = cmd_y;
            }
        }
    }
}

static void
rpi_pred_y_b(HEVCContext * const s,
           const int x0, const int y0,
           const int nPbW, const int nPbH,
           const struct MvField *const mv_field,
           AVFrame *const src_frame,
           AVFrame *const src_frame2)
{
    const unsigned int y_off = av_rpi_sand_frame_off_y(s->frame, x0, y0);
    const Mv * const mv  = mv_field->mv + 0;
    const Mv * const mv2 = mv_field->mv + 1;

    const unsigned int mx          = mv->x & 3;
    const unsigned int my          = mv->y & 3;
    const unsigned int my_mx = (my<<8) | mx;
    const unsigned int mx2          = mv2->x & 3;
    const unsigned int my2          = mv2->y & 3;
    const unsigned int my2_mx2 = (my2<<8) | mx2;
    const uint32_t     my2_mx2_my_mx = (my2_mx2 << 16) | my_mx;
    const unsigned int ref_idx0 = mv_field->ref_idx[0];
    const unsigned int ref_idx1 = mv_field->ref_idx[1];
    const uint32_t wt_offset =
        offset_depth_adj(s, s->sh.luma_offset_l0[ref_idx0] + s->sh.luma_offset_l1[ref_idx1]) + 1;
    const uint32_t wo1 = PACK2(wt_offset, s->sh.luma_weight_l0[ref_idx0]);
    const uint32_t wo2 = PACK2(wt_offset, s->sh.luma_weight_l1[ref_idx1]);

    const unsigned int xshl = av_rpi_sand_frame_xshl(s->frame);
    qpu_mc_dst_addr_t dst = get_mc_address_y(s->frame) + y_off;
    const qpu_mc_src_addr_t src1_base = get_mc_address_y(src_frame);
    const qpu_mc_src_addr_t src2_base = get_mc_address_y(src_frame2);
    HEVCRpiInterPredEnv * const ipe = &s->jb0->luma_ip;

    if (my2_mx2_my_mx == 0)
    {
        const int x1 = x0 + (mv->x >> 2);
        const int y1 = y0 + (mv->y >> 2);
        const int x2 = x0 + (mv2->x >> 2);
        const int y2 = y0 + (mv2->y >> 2);
        const int bh = nPbH;

        // Can do chunks a full 16 wide if we don't want the H filter
        for (int start_x=0; start_x < nPbW; start_x += 16)
        {
            HEVCRpiInterPredQ *const yp = rpi_nxt_pred(ipe, bh, s->qpu.y_b00);
            qpu_mc_src_t *const src1 = yp->last_l0;
            qpu_mc_src_t *const src2 = yp->last_l1;
            qpu_mc_pred_y_p_t *const cmd_y = &yp->qpu_mc_curr->y.p;
#if RPI_TSTATS
            {
                HEVCRpiStats *const ts = &s->tstats;
                ++ts->y_pred2_x0y0;

                if (nPbH > 16)
                    ++ts->y_pred2_hgt16;
                else
                    ++ts->y_pred2_hle16;
            }
#endif
            src1->x = x1 + start_x;
            src1->y = y1;
            src1->base = src1_base;
            src2->x = x2 + start_x;
            src2->y = y2;
            src2->base = src2_base;
            cmd_y->w = FFMIN(nPbW - start_x, 16);
            cmd_y->h = bh;
            cmd_y->mymx21 = 0;
            cmd_y->wo1 = wo1;
            cmd_y->wo2 = wo2;
            cmd_y->dst_addr =  dst + (start_x << xshl);
            yp->last_l0 = &cmd_y->next_src1;
            yp->last_l1 = &cmd_y->next_src2;
            *(qpu_mc_pred_y_p_t **)&yp->qpu_mc_curr = cmd_y + 1;
        }
    }
    else
    {
        // Filter requires a run-up of 3
        const int x1 = x0 + (mv->x >> 2) - 3;
        const int y1 = y0 + (mv->y >> 2) - 3;
        const int x2 = x0 + (mv2->x >> 2) - 3;
        const int y2 = y0 + (mv2->y >> 2) - 3;
        const int bh = nPbH;

        for (int start_x=0; start_x < nPbW; start_x += 8)
        { // B blocks work 8 at a time
            // B weights aren't doubled as the QPU code does the same
            // amount of work as it does for P
            HEVCRpiInterPredQ *const yp = rpi_nxt_pred(ipe, bh + 7, s->qpu.y_bxx);
            qpu_mc_src_t *const src1 = yp->last_l0;
            qpu_mc_src_t *const src2 = yp->last_l1;
            qpu_mc_pred_y_p_t *const cmd_y = &yp->qpu_mc_curr->y.p;
#if RPI_TSTATS
            {
                HEVCRpiStats *const ts = &s->tstats;
                const unsigned int mmx = mx | mx2;
                const unsigned int mmy = my | my2;
                if (mmx == 0 && mmy == 0)
                    ++ts->y_pred2_x0y0;
                else if (mmx == 0)
                    ++ts->y_pred2_x0;
                else if (mmy == 0)
                    ++ts->y_pred2_y0;
                else
                    ++ts->y_pred2_xy;

                if (nPbH > 16)
                    ++ts->y_pred2_hgt16;
                else
                    ++ts->y_pred2_hle16;
            }
#endif
            src1->x = x1 + start_x;
            src1->y = y1;
            src1->base = src1_base;
            src2->x = x2 + start_x;
            src2->y = y2;
            src2->base = src2_base;
            cmd_y->w = FFMIN(nPbW - start_x, 8);
            cmd_y->h = bh;
            cmd_y->mymx21 = my2_mx2_my_mx;
            cmd_y->wo1 = wo1;
            cmd_y->wo2 = wo2;
            cmd_y->dst_addr =  dst + (start_x << xshl);
            yp->last_l0 = &cmd_y->next_src1;
            yp->last_l1 = &cmd_y->next_src2;
            *(qpu_mc_pred_y_p_t **)&yp->qpu_mc_curr = cmd_y + 1;
        }
    }
}

// h/v shifts fixed at one as that is all the qasm copes with
static void
rpi_pred_c(HEVCContext * const s, const unsigned int lx, const int x0_c, const int y0_c,
  const int nPbW_c, const int nPbH_c,
  const Mv * const mv,
  const int16_t * const c_weights,
  const int16_t * const c_offsets,
  AVFrame * const src_frame)
{
    const unsigned int c_off = av_rpi_sand_frame_off_c(s->frame, x0_c, y0_c);
    const int hshift = 1; // = s->ps.sps->hshift[1];
    const int vshift = 1; // = s->ps.sps->vshift[1];

    const int x1_c = x0_c + (mv->x >> (2 + hshift)) - 1;
    const int y1_c = y0_c + (mv->y >> (2 + hshift)) - 1;
    const qpu_mc_src_addr_t src_base_u = get_mc_address_u(src_frame);
    const uint32_t x_coeffs = rpi_filter_coefs[av_mod_uintp2(mv->x, 2 + hshift) << (1 - hshift)];
    const uint32_t y_coeffs = rpi_filter_coefs[av_mod_uintp2(mv->y, 2 + vshift) << (1 - vshift)];
    const uint32_t wo_u = PACK2(offset_depth_adj(s, c_offsets[0]) * 2 + 1, c_weights[0]);
    const uint32_t wo_v = PACK2(offset_depth_adj(s, c_offsets[1]) * 2 + 1, c_weights[1]);
    qpu_mc_dst_addr_t dst_base_u = get_mc_address_u(s->frame) + c_off;
    HEVCRpiInterPredEnv * const ipe = &s->jb0->chroma_ip;
    const unsigned int xshl = av_rpi_sand_frame_xshl(s->frame) + 1;
    const unsigned int bh = nPbH_c;
    const uint32_t qfn = lx == 0 ? s->qpu.c_pxx : s->qpu.c_pxx_l1;

    for(int start_x=0; start_x < nPbW_c; start_x+=RPI_CHROMA_BLOCK_WIDTH)
    {
        HEVCRpiInterPredQ * const cp = rpi_nxt_pred(ipe, bh + 3, qfn);
        qpu_mc_pred_c_p_t * const cmd_c = &cp->qpu_mc_curr->c.p;
        qpu_mc_src_t ** const plast_lx = (lx == 0) ? &cp->last_l0 : &cp->last_l1;
        qpu_mc_src_t * const last_lx = *plast_lx;
        const int bw = FFMIN(nPbW_c-start_x, RPI_CHROMA_BLOCK_WIDTH);

        last_lx->x = x1_c + start_x;
        last_lx->y = y1_c;
        last_lx->base = src_base_u;
        cmd_c->h = bh;
        cmd_c->w = bw;
        cmd_c->coeffs_x = x_coeffs;
        cmd_c->coeffs_y = y_coeffs;
        cmd_c->wo_u = wo_u;
        cmd_c->wo_v = wo_v;
        cmd_c->dst_addr_c = dst_base_u + (start_x << xshl);
        *plast_lx = &cmd_c->next_src;
        *(qpu_mc_pred_c_p_t **)&cp->qpu_mc_curr = cmd_c + 1;
    }
    return;
}

// h/v shifts fixed at one as that is all the qasm copes with
static void
rpi_pred_c_b(HEVCContext * const s, const int x0_c, const int y0_c,
  const int nPbW_c, const int nPbH_c,
  const struct MvField * const mv_field,
  const int16_t * const c_weights,
  const int16_t * const c_offsets,
  const int16_t * const c_weights2,
  const int16_t * const c_offsets2,
  AVFrame * const src_frame,
  AVFrame * const src_frame2)
{
    const unsigned int c_off = av_rpi_sand_frame_off_c(s->frame, x0_c, y0_c);
    const int hshift = 1; // s->ps.sps->hshift[1];
    const int vshift = 1; // s->ps.sps->vshift[1];
    const Mv * const mv = mv_field->mv + 0;
    const Mv * const mv2 = mv_field->mv + 1;

    const unsigned int mx = av_mod_uintp2(mv->x, 2 + hshift);
    const unsigned int my = av_mod_uintp2(mv->y, 2 + vshift);
    const uint32_t coefs0_x = rpi_filter_coefs[mx << (1 - hshift)];
    const uint32_t coefs0_y = rpi_filter_coefs[my << (1 - vshift)]; // Fractional part of motion vector
    const int x1_c = x0_c + (mv->x >> (2 + hshift)) - 1;
    const int y1_c = y0_c + (mv->y >> (2 + hshift)) - 1;

    const unsigned int mx2 = av_mod_uintp2(mv2->x, 2 + hshift);
    const unsigned int my2 = av_mod_uintp2(mv2->y, 2 + vshift);
    const uint32_t coefs1_x = rpi_filter_coefs[mx2 << (1 - hshift)];
    const uint32_t coefs1_y = rpi_filter_coefs[my2 << (1 - vshift)]; // Fractional part of motion vector

    const int x2_c = x0_c + (mv2->x >> (2 + hshift)) - 1;
    const int y2_c = y0_c + (mv2->y >> (2 + hshift)) - 1;

    const uint32_t wo_u2 = PACK2(offset_depth_adj(s, c_offsets[0] + c_offsets2[0]) + 1, c_weights2[0]);
    const uint32_t wo_v2 = PACK2(offset_depth_adj(s, c_offsets[1] + c_offsets2[1]) + 1, c_weights2[1]);

    const qpu_mc_dst_addr_t dst_base_u = get_mc_address_u(s->frame) + c_off;
    const qpu_mc_src_addr_t src1_base = get_mc_address_u(src_frame);
    const qpu_mc_src_addr_t src2_base = get_mc_address_u(src_frame2);
    HEVCRpiInterPredEnv * const ipe = &s->jb0->chroma_ip;
    const unsigned int xshl = av_rpi_sand_frame_xshl(s->frame) + 1;
    const unsigned int bh = nPbH_c;

    for (int start_x=0; start_x < nPbW_c; start_x += RPI_CHROMA_BLOCK_WIDTH)
    {
        const unsigned int bw = FFMIN(nPbW_c-start_x, RPI_CHROMA_BLOCK_WIDTH);

        HEVCRpiInterPredQ * const cp = rpi_nxt_pred(ipe, bh * 2 + 3, s->qpu.c_bxx);
        qpu_mc_pred_c_b_t * const u = &cp->qpu_mc_curr->c.b;
        qpu_mc_src_t * const src_l0 = cp->last_l0;
        qpu_mc_src_t * const src_l1 = cp->last_l1;

        src_l0->x = x1_c + start_x;
        src_l0->y = y1_c;
        src_l0->base = src1_base;
        src_l1->x = x2_c + start_x;
        src_l1->y = y2_c;
        src_l1->base = src2_base;

        u[0].h = bh;
        u[0].w = bw;
        u[0].coeffs_x1 = coefs0_x;
        u[0].coeffs_y1 = coefs0_y;
        u[0].weight_u1 = c_weights[0]; // Weight L0 U
        u[0].weight_v1 = c_weights[1]; // Weight L0 V
        u[0].coeffs_x2 = coefs1_x;
        u[0].coeffs_y2 = coefs1_y;
        u[0].wo_u2 = wo_u2;
        u[0].wo_v2 = wo_v2;
        u[0].dst_addr_c = dst_base_u + (start_x << xshl);

        cp->last_l0 = &u[0].next_src1;
        cp->last_l1 = &u[0].next_src2;
        *(qpu_mc_pred_c_b_t **)&cp->qpu_mc_curr = u + 1;
    }
}


#endif



static void hls_prediction_unit(HEVCContext * const s, const int x0, const int y0,
                                const int nPbW, const int nPbH,
                                const unsigned int log2_cb_size, const unsigned int partIdx, const unsigned int idx)
{
#define POS(c_idx, x, y)                                                              \
    &s->frame->data[c_idx][((y) >> s->ps.sps->vshift[c_idx]) * s->frame->linesize[c_idx] + \
                           (((x) >> s->ps.sps->hshift[c_idx]) << s->ps.sps->pixel_shift)]
    HEVCLocalContext * const lc = s->HEVClc;
    int merge_idx = 0;
    struct MvField current_mv = {{{ 0 }}};

    int min_pu_width = s->ps.sps->min_pu_width;

    MvField *tab_mvf = s->ref->tab_mvf;
    RefPicList  *refPicList = s->ref->refPicList;
    HEVCFrame *ref0 = NULL, *ref1 = NULL;
    uint8_t *dst0 = POS(0, x0, y0);
    uint8_t *dst1 = POS(1, x0, y0);
    uint8_t *dst2 = POS(2, x0, y0);
    int log2_min_cb_size = s->ps.sps->log2_min_cb_size;
    int min_cb_width     = s->ps.sps->min_cb_width;
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;
    int x_pu, y_pu;
    int i, j;
    const int skip_flag = SAMPLE_CTB(s->skip_flag, x_cb, y_cb);

    if (!skip_flag)
        lc->pu.merge_flag = ff_hevc_merge_flag_decode(s);

    if (skip_flag || lc->pu.merge_flag) {
        if (s->sh.max_num_merge_cand > 1)
            merge_idx = ff_hevc_merge_idx_decode(s);
        else
            merge_idx = 0;

        ff_hevc_luma_mv_merge_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                                   partIdx, merge_idx, &current_mv);
    } else {
        hevc_luma_mv_mvp_mode(s, x0, y0, nPbW, nPbH, log2_cb_size,
                              partIdx, merge_idx, &current_mv);
    }

    x_pu = x0 >> s->ps.sps->log2_min_pu_size;
    y_pu = y0 >> s->ps.sps->log2_min_pu_size;

    for (j = 0; j < nPbH >> s->ps.sps->log2_min_pu_size; j++)
        for (i = 0; i < nPbW >> s->ps.sps->log2_min_pu_size; i++)
            tab_mvf[(y_pu + j) * min_pu_width + x_pu + i] = current_mv;

    if (current_mv.pred_flag & PF_L0) {
        ref0 = refPicList[0].ref[current_mv.ref_idx[0]];
        if (!ref0)
            return;
        threadlog_progress_type = threadlog_reason_AWAIT_PROGRESS_PRED_L0;
        hevc_await_progress(s, ref0, &current_mv.mv[0], y0, nPbH);
    }
    if (current_mv.pred_flag & PF_L1) {
        ref1 = refPicList[1].ref[current_mv.ref_idx[1]];
        if (!ref1)
            return;
        threadlog_progress_type = threadlog_reason_AWAIT_PROGRESS_PRED_L1;
        hevc_await_progress(s, ref1, &current_mv.mv[1], y0, nPbH);
    }

    if (current_mv.pred_flag == PF_L0) {
        int x0_c = x0 >> s->ps.sps->hshift[1];
        int y0_c = y0 >> s->ps.sps->vshift[1];
        int nPbW_c = nPbW >> s->ps.sps->hshift[1];
        int nPbH_c = nPbH >> s->ps.sps->vshift[1];

#if RPI_INTER
        if (s->enable_rpi) {
            rpi_pred_y(s, x0, y0, nPbW, nPbH, current_mv.mv + 0,
              s->sh.luma_weight_l0[current_mv.ref_idx[0]], s->sh.luma_offset_l0[current_mv.ref_idx[0]],
              ref0->frame);
        } else
#endif
        {
            luma_mc_uni(s, dst0, s->frame->linesize[0], ref0->frame,
                    &current_mv.mv[0], x0, y0, nPbW, nPbH,
                    s->sh.luma_weight_l0[current_mv.ref_idx[0]],
                    s->sh.luma_offset_l0[current_mv.ref_idx[0]]);
        }

        if (s->ps.sps->chroma_format_idc) {
#if RPI_INTER
            if (s->enable_rpi) {
                rpi_pred_c(s, 0, x0_c, y0_c, nPbW_c, nPbH_c, current_mv.mv + 0,
                  s->sh.chroma_weight_l0[current_mv.ref_idx[0]], s->sh.chroma_offset_l0[current_mv.ref_idx[0]],
                  ref0->frame);
                return;
            }
#endif
            chroma_mc_uni(s, dst1, s->frame->linesize[1], ref0->frame->data[1], ref0->frame->linesize[1],
                          0, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l0[current_mv.ref_idx[0]][0], s->sh.chroma_offset_l0[current_mv.ref_idx[0]][0]);
            chroma_mc_uni(s, dst2, s->frame->linesize[2], ref0->frame->data[2], ref0->frame->linesize[2],
                          0, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l0[current_mv.ref_idx[0]][1], s->sh.chroma_offset_l0[current_mv.ref_idx[0]][1]);
        }
    } else if (current_mv.pred_flag == PF_L1) {
        int x0_c = x0 >> s->ps.sps->hshift[1];
        int y0_c = y0 >> s->ps.sps->vshift[1];
        int nPbW_c = nPbW >> s->ps.sps->hshift[1];
        int nPbH_c = nPbH >> s->ps.sps->vshift[1];

#if RPI_INTER
        if (s->enable_rpi) {
            rpi_pred_y(s, x0, y0, nPbW, nPbH, current_mv.mv + 1,
              s->sh.luma_weight_l1[current_mv.ref_idx[1]], s->sh.luma_offset_l1[current_mv.ref_idx[1]],
              ref1->frame);
        } else
#endif
        {
            luma_mc_uni(s, dst0, s->frame->linesize[0], ref1->frame,
                    &current_mv.mv[1], x0, y0, nPbW, nPbH,
                    s->sh.luma_weight_l1[current_mv.ref_idx[1]],
                    s->sh.luma_offset_l1[current_mv.ref_idx[1]]);
        }

        if (s->ps.sps->chroma_format_idc) {
#if RPI_INTER
            if (s->enable_rpi) {
                rpi_pred_c(s, 1, x0_c, y0_c, nPbW_c, nPbH_c, current_mv.mv + 1,
                  s->sh.chroma_weight_l1[current_mv.ref_idx[1]], s->sh.chroma_offset_l1[current_mv.ref_idx[1]],
                  ref1->frame);
                return;
            }
#endif
            chroma_mc_uni(s, dst1, s->frame->linesize[1], ref1->frame->data[1], ref1->frame->linesize[1],
                          1, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l1[current_mv.ref_idx[1]][0], s->sh.chroma_offset_l1[current_mv.ref_idx[1]][0]);

            chroma_mc_uni(s, dst2, s->frame->linesize[2], ref1->frame->data[2], ref1->frame->linesize[2],
                          1, x0_c, y0_c, nPbW_c, nPbH_c, &current_mv,
                          s->sh.chroma_weight_l1[current_mv.ref_idx[1]][1], s->sh.chroma_offset_l1[current_mv.ref_idx[1]][1]);
        }
    } else if (current_mv.pred_flag == PF_BI) {
        int x0_c = x0 >> s->ps.sps->hshift[1];
        int y0_c = y0 >> s->ps.sps->vshift[1];
        int nPbW_c = nPbW >> s->ps.sps->hshift[1];
        int nPbH_c = nPbH >> s->ps.sps->vshift[1];

#if RPI_INTER
        if (s->enable_rpi) {
            rpi_pred_y_b(s, x0, y0, nPbW, nPbH, &current_mv, ref0->frame, ref1->frame);
        } else
#endif
        {
            luma_mc_bi(s, dst0, s->frame->linesize[0], ref0->frame,
                   &current_mv.mv[0], x0, y0, nPbW, nPbH,
                   ref1->frame, &current_mv.mv[1], &current_mv);
        }

        if (s->ps.sps->chroma_format_idc) {
#if RPI_INTER
          if (s->enable_rpi) {
              rpi_pred_c_b(s, x0_c, y0_c, nPbW_c, nPbH_c,
                           &current_mv,
                           s->sh.chroma_weight_l0[current_mv.ref_idx[0]],
                           s->sh.chroma_offset_l0[current_mv.ref_idx[0]],
                           s->sh.chroma_weight_l1[current_mv.ref_idx[1]],
                           s->sh.chroma_offset_l1[current_mv.ref_idx[1]],
                           ref0->frame,
                           ref1->frame);
                return;
            }
#endif
            chroma_mc_bi(s, dst1, s->frame->linesize[1], ref0->frame, ref1->frame,
                         x0_c, y0_c, nPbW_c, nPbH_c, &current_mv, 0);

            chroma_mc_bi(s, dst2, s->frame->linesize[2], ref0->frame, ref1->frame,
                         x0_c, y0_c, nPbW_c, nPbH_c, &current_mv, 1);
        }
    }
}

/**
 * 8.4.1
 */
static int luma_intra_pred_mode(HEVCContext *s, int x0, int y0, int pu_size,
                                int prev_intra_luma_pred_flag)
{
    HEVCLocalContext *lc = s->HEVClc;
    int x_pu             = x0 >> s->ps.sps->log2_min_pu_size;
    int y_pu             = y0 >> s->ps.sps->log2_min_pu_size;
    int min_pu_width     = s->ps.sps->min_pu_width;
    int size_in_pus      = pu_size >> s->ps.sps->log2_min_pu_size;
    int x0b              = av_mod_uintp2(x0, s->ps.sps->log2_ctb_size);
    int y0b              = av_mod_uintp2(y0, s->ps.sps->log2_ctb_size);

    int cand_up   = (lc->ctb_up_flag || y0b) ?
                    s->tab_ipm[(y_pu - 1) * min_pu_width + x_pu] : INTRA_DC;
    int cand_left = (lc->ctb_left_flag || x0b) ?
                    s->tab_ipm[y_pu * min_pu_width + x_pu - 1]   : INTRA_DC;

    int y_ctb = (y0 >> (s->ps.sps->log2_ctb_size)) << (s->ps.sps->log2_ctb_size);

    MvField *tab_mvf = s->ref->tab_mvf;
    int intra_pred_mode;
    int candidate[3];
    int i, j;

    // intra_pred_mode prediction does not cross vertical CTB boundaries
    if ((y0 - 1) < y_ctb)
        cand_up = INTRA_DC;

    if (cand_left == cand_up) {
        if (cand_left < 2) {
            candidate[0] = INTRA_PLANAR;
            candidate[1] = INTRA_DC;
            candidate[2] = INTRA_ANGULAR_26;
        } else {
            candidate[0] = cand_left;
            candidate[1] = 2 + ((cand_left - 2 - 1 + 32) & 31);
            candidate[2] = 2 + ((cand_left - 2 + 1) & 31);
        }
    } else {
        candidate[0] = cand_left;
        candidate[1] = cand_up;
        if (candidate[0] != INTRA_PLANAR && candidate[1] != INTRA_PLANAR) {
            candidate[2] = INTRA_PLANAR;
        } else if (candidate[0] != INTRA_DC && candidate[1] != INTRA_DC) {
            candidate[2] = INTRA_DC;
        } else {
            candidate[2] = INTRA_ANGULAR_26;
        }
    }

    if (prev_intra_luma_pred_flag) {
        intra_pred_mode = candidate[lc->pu.mpm_idx];
    } else {
        if (candidate[0] > candidate[1])
            FFSWAP(uint8_t, candidate[0], candidate[1]);
        if (candidate[0] > candidate[2])
            FFSWAP(uint8_t, candidate[0], candidate[2]);
        if (candidate[1] > candidate[2])
            FFSWAP(uint8_t, candidate[1], candidate[2]);

        intra_pred_mode = lc->pu.rem_intra_luma_pred_mode;
        for (i = 0; i < 3; i++)
            if (intra_pred_mode >= candidate[i])
                intra_pred_mode++;
    }

    /* write the intra prediction units into the mv array */
    if (!size_in_pus)
        size_in_pus = 1;
    for (i = 0; i < size_in_pus; i++) {
        memset(&s->tab_ipm[(y_pu + i) * min_pu_width + x_pu],
               intra_pred_mode, size_in_pus);

        for (j = 0; j < size_in_pus; j++) {
            tab_mvf[(y_pu + j) * min_pu_width + x_pu + i].pred_flag = PF_INTRA;
        }
    }

    return intra_pred_mode;
}

static av_always_inline void set_ct_depth(HEVCContext *s, int x0, int y0,
                                          int log2_cb_size, int ct_depth)
{
    int length = (1 << log2_cb_size) >> s->ps.sps->log2_min_cb_size;
    int x_cb   = x0 >> s->ps.sps->log2_min_cb_size;
    int y_cb   = y0 >> s->ps.sps->log2_min_cb_size;
    int y;

    for (y = 0; y < length; y++)
        memset(&s->tab_ct_depth[(y_cb + y) * s->ps.sps->min_cb_width + x_cb],
               ct_depth, length);
}

static const uint8_t tab_mode_idx[] = {
     0,  1,  2,  2,  2,  2,  3,  5,  7,  8, 10, 12, 13, 15, 17, 18, 19, 20,
    21, 22, 23, 23, 24, 24, 25, 25, 26, 27, 27, 28, 28, 29, 29, 30, 31};

static void intra_prediction_unit(HEVCContext *s, int x0, int y0,
                                  int log2_cb_size)
{
    HEVCLocalContext *lc = s->HEVClc;
    static const uint8_t intra_chroma_table[4] = { 0, 26, 10, 1 };
    uint8_t prev_intra_luma_pred_flag[4];
    int split   = lc->cu.part_mode == PART_NxN;
    int pb_size = (1 << log2_cb_size) >> split;
    int side    = split + 1;
    int chroma_mode;
    int i, j;

    for (i = 0; i < side; i++)
        for (j = 0; j < side; j++)
            prev_intra_luma_pred_flag[2 * i + j] = ff_hevc_prev_intra_luma_pred_flag_decode(s);

    for (i = 0; i < side; i++) {
        for (j = 0; j < side; j++) {
            if (prev_intra_luma_pred_flag[2 * i + j])
                lc->pu.mpm_idx = ff_hevc_mpm_idx_decode(s);
            else
                lc->pu.rem_intra_luma_pred_mode = ff_hevc_rem_intra_luma_pred_mode_decode(s);

            lc->pu.intra_pred_mode[2 * i + j] =
                luma_intra_pred_mode(s, x0 + pb_size * j, y0 + pb_size * i, pb_size,
                                     prev_intra_luma_pred_flag[2 * i + j]);
        }
    }

    if (s->ps.sps->chroma_format_idc == 3) {
        for (i = 0; i < side; i++) {
            for (j = 0; j < side; j++) {
                lc->pu.chroma_mode_c[2 * i + j] = chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
                if (chroma_mode != 4) {
                    if (lc->pu.intra_pred_mode[2 * i + j] == intra_chroma_table[chroma_mode])
                        lc->pu.intra_pred_mode_c[2 * i + j] = 34;
                    else
                        lc->pu.intra_pred_mode_c[2 * i + j] = intra_chroma_table[chroma_mode];
                } else {
                    lc->pu.intra_pred_mode_c[2 * i + j] = lc->pu.intra_pred_mode[2 * i + j];
                }
            }
        }
    } else if (s->ps.sps->chroma_format_idc == 2) {
        int mode_idx;
        lc->pu.chroma_mode_c[0] = chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
        if (chroma_mode != 4) {
            if (lc->pu.intra_pred_mode[0] == intra_chroma_table[chroma_mode])
                mode_idx = 34;
            else
                mode_idx = intra_chroma_table[chroma_mode];
        } else {
            mode_idx = lc->pu.intra_pred_mode[0];
        }
        lc->pu.intra_pred_mode_c[0] = tab_mode_idx[mode_idx];
    } else if (s->ps.sps->chroma_format_idc != 0) {
        chroma_mode = ff_hevc_intra_chroma_pred_mode_decode(s);
        if (chroma_mode != 4) {
            if (lc->pu.intra_pred_mode[0] == intra_chroma_table[chroma_mode])
                lc->pu.intra_pred_mode_c[0] = 34;
            else
                lc->pu.intra_pred_mode_c[0] = intra_chroma_table[chroma_mode];
        } else {
            lc->pu.intra_pred_mode_c[0] = lc->pu.intra_pred_mode[0];
        }
    }
}

static void intra_prediction_unit_default_value(HEVCContext *s,
                                                int x0, int y0,
                                                int log2_cb_size)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pb_size          = 1 << log2_cb_size;
    int size_in_pus      = pb_size >> s->ps.sps->log2_min_pu_size;
    int min_pu_width     = s->ps.sps->min_pu_width;
    MvField *tab_mvf     = s->ref->tab_mvf;
    int x_pu             = x0 >> s->ps.sps->log2_min_pu_size;
    int y_pu             = y0 >> s->ps.sps->log2_min_pu_size;
    int j, k;

    if (size_in_pus == 0)
        size_in_pus = 1;
    for (j = 0; j < size_in_pus; j++)
        memset(&s->tab_ipm[(y_pu + j) * min_pu_width + x_pu], INTRA_DC, size_in_pus);
    if (lc->cu.pred_mode == MODE_INTRA)
        for (j = 0; j < size_in_pus; j++)
            for (k = 0; k < size_in_pus; k++)
                tab_mvf[(y_pu + j) * min_pu_width + x_pu + k].pred_flag = PF_INTRA;
}

static int hls_coding_unit(HEVCContext *s, int x0, int y0, int log2_cb_size)
{
    int cb_size          = 1 << log2_cb_size;
    HEVCLocalContext *lc = s->HEVClc;
    int log2_min_cb_size = s->ps.sps->log2_min_cb_size;
    int length           = cb_size >> log2_min_cb_size;
    int min_cb_width     = s->ps.sps->min_cb_width;
    int x_cb             = x0 >> log2_min_cb_size;
    int y_cb             = y0 >> log2_min_cb_size;
    int idx              = log2_cb_size - 2;
    int qp_block_mask    = (1<<(s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_qp_delta_depth)) - 1;
    int x, y, ret;

    lc->cu.x                = x0;
    lc->cu.y                = y0;
    lc->cu.pred_mode        = MODE_INTRA;
    lc->cu.part_mode        = PART_2Nx2N;
    lc->cu.intra_split_flag = 0;

    SAMPLE_CTB(s->skip_flag, x_cb, y_cb) = 0;
    for (x = 0; x < 4; x++)
        lc->pu.intra_pred_mode[x] = 1;
    if (s->ps.pps->transquant_bypass_enable_flag) {
        lc->cu.cu_transquant_bypass_flag = ff_hevc_cu_transquant_bypass_flag_decode(s);
        if (lc->cu.cu_transquant_bypass_flag)
            set_deblocking_bypass(s, x0, y0, log2_cb_size);
    } else
        lc->cu.cu_transquant_bypass_flag = 0;

    if (s->sh.slice_type != HEVC_SLICE_I) {
        uint8_t skip_flag = ff_hevc_skip_flag_decode(s, x0, y0, x_cb, y_cb);

        x = y_cb * min_cb_width + x_cb;
        for (y = 0; y < length; y++) {
            memset(&s->skip_flag[x], skip_flag, length);
            x += min_cb_width;
        }
        lc->cu.pred_mode = skip_flag ? MODE_SKIP : MODE_INTER;
    } else {
        x = y_cb * min_cb_width + x_cb;
        for (y = 0; y < length; y++) {
            memset(&s->skip_flag[x], 0, length);
            x += min_cb_width;
        }
    }

    if (SAMPLE_CTB(s->skip_flag, x_cb, y_cb)) {
        hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, idx);
        intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);

        if (!s->sh.disable_deblocking_filter_flag)
            ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
    } else {
        int pcm_flag = 0;

        if (s->sh.slice_type != HEVC_SLICE_I)
            lc->cu.pred_mode = ff_hevc_pred_mode_decode(s);
        if (lc->cu.pred_mode != MODE_INTRA ||
            log2_cb_size == s->ps.sps->log2_min_cb_size) {
            lc->cu.part_mode        = ff_hevc_part_mode_decode(s, log2_cb_size);
            lc->cu.intra_split_flag = lc->cu.part_mode == PART_NxN &&
                                      lc->cu.pred_mode == MODE_INTRA;
        }

        if (lc->cu.pred_mode == MODE_INTRA) {
            if (lc->cu.part_mode == PART_2Nx2N && s->ps.sps->pcm_enabled_flag &&
                log2_cb_size >= s->ps.sps->pcm.log2_min_pcm_cb_size &&
                log2_cb_size <= s->ps.sps->pcm.log2_max_pcm_cb_size) {
                pcm_flag = ff_hevc_pcm_flag_decode(s);
            }
            if (pcm_flag) {
                intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);
                ret = hls_pcm_sample(s, x0, y0, log2_cb_size);
                if (s->ps.sps->pcm.loop_filter_disable_flag)
                {
                    set_deblocking_bypass(s, x0, y0, log2_cb_size);
                }

                if (ret < 0)
                    return ret;
            } else {
                intra_prediction_unit(s, x0, y0, log2_cb_size);
            }
        } else {
            intra_prediction_unit_default_value(s, x0, y0, log2_cb_size);
            switch (lc->cu.part_mode) {
            case PART_2Nx2N:
                hls_prediction_unit(s, x0, y0, cb_size, cb_size, log2_cb_size, 0, idx);
                break;
            case PART_2NxN:
                hls_prediction_unit(s, x0, y0,               cb_size, cb_size / 2, log2_cb_size, 0, idx);
                hls_prediction_unit(s, x0, y0 + cb_size / 2, cb_size, cb_size / 2, log2_cb_size, 1, idx);
                break;
            case PART_Nx2N:
                hls_prediction_unit(s, x0,               y0, cb_size / 2, cb_size, log2_cb_size, 0, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0, cb_size / 2, cb_size, log2_cb_size, 1, idx - 1);
                break;
            case PART_2NxnU:
                hls_prediction_unit(s, x0, y0,               cb_size, cb_size     / 4, log2_cb_size, 0, idx);
                hls_prediction_unit(s, x0, y0 + cb_size / 4, cb_size, cb_size * 3 / 4, log2_cb_size, 1, idx);
                break;
            case PART_2NxnD:
                hls_prediction_unit(s, x0, y0,                   cb_size, cb_size * 3 / 4, log2_cb_size, 0, idx);
                hls_prediction_unit(s, x0, y0 + cb_size * 3 / 4, cb_size, cb_size     / 4, log2_cb_size, 1, idx);
                break;
            case PART_nLx2N:
                hls_prediction_unit(s, x0,               y0, cb_size     / 4, cb_size, log2_cb_size, 0, idx - 2);
                hls_prediction_unit(s, x0 + cb_size / 4, y0, cb_size * 3 / 4, cb_size, log2_cb_size, 1, idx - 2);
                break;
            case PART_nRx2N:
                hls_prediction_unit(s, x0,                   y0, cb_size * 3 / 4, cb_size, log2_cb_size, 0, idx - 2);
                hls_prediction_unit(s, x0 + cb_size * 3 / 4, y0, cb_size     / 4, cb_size, log2_cb_size, 1, idx - 2);
                break;
            case PART_NxN:
                hls_prediction_unit(s, x0,               y0,               cb_size / 2, cb_size / 2, log2_cb_size, 0, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0,               cb_size / 2, cb_size / 2, log2_cb_size, 1, idx - 1);
                hls_prediction_unit(s, x0,               y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 2, idx - 1);
                hls_prediction_unit(s, x0 + cb_size / 2, y0 + cb_size / 2, cb_size / 2, cb_size / 2, log2_cb_size, 3, idx - 1);
                break;
            }
        }

        if (!pcm_flag) {
            int rqt_root_cbf = 1;

            if (lc->cu.pred_mode != MODE_INTRA &&
                !(lc->cu.part_mode == PART_2Nx2N && lc->pu.merge_flag)) {
                rqt_root_cbf = ff_hevc_no_residual_syntax_flag_decode(s);
            }
            if (rqt_root_cbf) {
                const static int cbf[2] = { 0 };
                lc->cu.max_trafo_depth = lc->cu.pred_mode == MODE_INTRA ?
                                         s->ps.sps->max_transform_hierarchy_depth_intra + lc->cu.intra_split_flag :
                                         s->ps.sps->max_transform_hierarchy_depth_inter;
                ret = hls_transform_tree(s, x0, y0, x0, y0, x0, y0,
                                         log2_cb_size,
                                         log2_cb_size, 0, 0, cbf, cbf);
                if (ret < 0)
                    return ret;
            } else {
                if (!s->sh.disable_deblocking_filter_flag)
                    ff_hevc_deblocking_boundary_strengths(s, x0, y0, log2_cb_size);
            }
        }
    }

    if (s->ps.pps->cu_qp_delta_enabled_flag && lc->tu.is_cu_qp_delta_coded == 0)
        ff_hevc_set_qPy(s, x0, y0, log2_cb_size);

    x = y_cb * min_cb_width + x_cb;
    for (y = 0; y < length; y++) {
        memset(&s->qp_y_tab[x], lc->qp_y, length);
        x += min_cb_width;
    }

    if(((x0 + (1<<log2_cb_size)) & qp_block_mask) == 0 &&
       ((y0 + (1<<log2_cb_size)) & qp_block_mask) == 0) {
        lc->qPy_pred = lc->qp_y;
    }

    set_ct_depth(s, x0, y0, log2_cb_size, lc->ct_depth);

    return 0;
}

static int hls_coding_quadtree(HEVCContext *s, int x0, int y0,
                               int log2_cb_size, int cb_depth)
{
    HEVCLocalContext *lc = s->HEVClc;
    const int cb_size    = 1 << log2_cb_size;
    int ret;
    int split_cu;

    lc->ct_depth = cb_depth;
    if (x0 + cb_size <= s->ps.sps->width  &&
        y0 + cb_size <= s->ps.sps->height &&
        log2_cb_size > s->ps.sps->log2_min_cb_size) {
        split_cu = ff_hevc_split_coding_unit_flag_decode(s, cb_depth, x0, y0);
    } else {
        split_cu = (log2_cb_size > s->ps.sps->log2_min_cb_size);
    }
    if (s->ps.pps->cu_qp_delta_enabled_flag &&
        log2_cb_size >= s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_qp_delta_depth) {
        lc->tu.is_cu_qp_delta_coded = 0;
        lc->tu.cu_qp_delta          = 0;
    }

    if (s->sh.cu_chroma_qp_offset_enabled_flag &&
        log2_cb_size >= s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_chroma_qp_offset_depth) {
        lc->tu.is_cu_chroma_qp_offset_coded = 0;
    }

    if (split_cu) {
        int qp_block_mask = (1<<(s->ps.sps->log2_ctb_size - s->ps.pps->diff_cu_qp_delta_depth)) - 1;
        const int cb_size_split = cb_size >> 1;
        const int x1 = x0 + cb_size_split;
        const int y1 = y0 + cb_size_split;

        int more_data = 0;

        more_data = hls_coding_quadtree(s, x0, y0, log2_cb_size - 1, cb_depth + 1);
        if (more_data < 0)
            return more_data;

        if (more_data && x1 < s->ps.sps->width) {
            more_data = hls_coding_quadtree(s, x1, y0, log2_cb_size - 1, cb_depth + 1);
            if (more_data < 0)
                return more_data;
        }
        if (more_data && y1 < s->ps.sps->height) {
            more_data = hls_coding_quadtree(s, x0, y1, log2_cb_size - 1, cb_depth + 1);
            if (more_data < 0)
                return more_data;
        }
        if (more_data && x1 < s->ps.sps->width &&
            y1 < s->ps.sps->height) {
            more_data = hls_coding_quadtree(s, x1, y1, log2_cb_size - 1, cb_depth + 1);
            if (more_data < 0)
                return more_data;
        }

        if(((x0 + (1<<log2_cb_size)) & qp_block_mask) == 0 &&
            ((y0 + (1<<log2_cb_size)) & qp_block_mask) == 0)
            lc->qPy_pred = lc->qp_y;

        if (more_data)
            return ((x1 + cb_size_split) < s->ps.sps->width ||
                    (y1 + cb_size_split) < s->ps.sps->height);
        else
            return 0;
    } else {
        ret = hls_coding_unit(s, x0, y0, log2_cb_size);
        if (ret < 0)
            return ret;
        if ((!((x0 + cb_size) %
               (1 << (s->ps.sps->log2_ctb_size))) ||
             (x0 + cb_size >= s->ps.sps->width)) &&
            (!((y0 + cb_size) %
               (1 << (s->ps.sps->log2_ctb_size))) ||
             (y0 + cb_size >= s->ps.sps->height))) {
            int end_of_slice_flag = ff_hevc_end_of_slice_flag_decode(s);
            return !end_of_slice_flag;
        } else {
            return 1;
        }
    }

    return 0;
}

static void hls_decode_neighbour(HEVCContext *s, int x_ctb, int y_ctb,
                                 int ctb_addr_ts)
{
    HEVCLocalContext *lc  = s->HEVClc;
    int ctb_size          = 1 << s->ps.sps->log2_ctb_size;
    int ctb_addr_rs       = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts];
    int ctb_addr_in_slice = ctb_addr_rs - s->sh.slice_addr;

    s->tab_slice_address[ctb_addr_rs] = s->sh.slice_addr;

    if (s->ps.pps->entropy_coding_sync_enabled_flag) {
        if (x_ctb == 0 && (y_ctb & (ctb_size - 1)) == 0)
            lc->first_qp_group = 1;
        lc->end_of_tiles_x = s->ps.sps->width;
    } else if (s->ps.pps->tiles_enabled_flag) {
        if (ctb_addr_ts && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[ctb_addr_ts - 1]) {
            int idxX = s->ps.pps->col_idxX[x_ctb >> s->ps.sps->log2_ctb_size];
            lc->end_of_tiles_x   = x_ctb + (s->ps.pps->column_width[idxX] << s->ps.sps->log2_ctb_size);
            lc->first_qp_group   = 1;
        }
    } else {
        lc->end_of_tiles_x = s->ps.sps->width;
    }

    lc->end_of_tiles_y = FFMIN(y_ctb + ctb_size, s->ps.sps->height);

    lc->boundary_flags = 0;
    if (s->ps.pps->tiles_enabled_flag) {
        if (x_ctb > 0 && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs - 1]])
            lc->boundary_flags |= BOUNDARY_LEFT_TILE;
        if (x_ctb > 0 && s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - 1])
            lc->boundary_flags |= BOUNDARY_LEFT_SLICE;
        if (y_ctb > 0 && s->ps.pps->tile_id[ctb_addr_ts] != s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs - s->ps.sps->ctb_width]])
            lc->boundary_flags |= BOUNDARY_UPPER_TILE;
        if (y_ctb > 0 && s->tab_slice_address[ctb_addr_rs] != s->tab_slice_address[ctb_addr_rs - s->ps.sps->ctb_width])
            lc->boundary_flags |= BOUNDARY_UPPER_SLICE;
    } else {
        if (ctb_addr_in_slice <= 0)
            lc->boundary_flags |= BOUNDARY_LEFT_SLICE;
        if (ctb_addr_in_slice < s->ps.sps->ctb_width)
            lc->boundary_flags |= BOUNDARY_UPPER_SLICE;
    }

    lc->ctb_left_flag = ((x_ctb > 0) && (ctb_addr_in_slice > 0) && !(lc->boundary_flags & BOUNDARY_LEFT_TILE));
    lc->ctb_up_flag   = ((y_ctb > 0) && (ctb_addr_in_slice >= s->ps.sps->ctb_width) && !(lc->boundary_flags & BOUNDARY_UPPER_TILE));
    lc->ctb_up_right_flag = ((y_ctb > 0)  && (ctb_addr_in_slice+1 >= s->ps.sps->ctb_width) && (s->ps.pps->tile_id[ctb_addr_ts] == s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs+1 - s->ps.sps->ctb_width]]));
    lc->ctb_up_left_flag = ((x_ctb > 0) && (y_ctb > 0)  && (ctb_addr_in_slice-1 >= s->ps.sps->ctb_width) && (s->ps.pps->tile_id[ctb_addr_ts] == s->ps.pps->tile_id[s->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs-1 - s->ps.sps->ctb_width]]));
}

#ifdef RPI
static void rpi_execute_dblk_cmds(HEVCContext *s)
{
    const unsigned int ctb_size    = 1 << s->ps.sps->log2_ctb_size;
    HEVCRpiDeblkEnv *const de = &s->jb1->deblk;
    unsigned int i;

    for (i = 0; i != de->n; ++i)
    {
        ff_hevc_hls_filters(s, de->blks[i].x_ctb, de->blks[i].y_ctb, ctb_size);
    }
    de->n = 0;
}

#if 0
static void rpi_execute_transform(HEVCContext *s)
{
    int i=2;
    int job = s->pass1_job;
    /*int j;
    int16_t *coeffs = s->coeffs_buf_arm[job][i];
    for(j=s->num_coeffs[job][i]; j > 0; j-= 16*16, coeffs+=16*16) {
        s->hevcdsp.idct[4-2](coeffs, 16);
    }
    i=3;
    coeffs = s->coeffs_buf_arm[job][i] - s->num_coeffs[job][i];
    for(j=s->num_coeffs[job][i]; j > 0; j-= 32*32, coeffs+=32*32) {
        s->hevcdsp.idct[5-2](coeffs, 32);
    }*/

    rpi_cache_flush_one_gm_ptr(&s->coeffs_buf_accelerated[job], RPI_CACHE_FLUSH_MODE_WB_INVALIDATE);
    s->vpu_id = vpu_post_code2( vpu_get_fn(), vpu_get_constants(), s->coeffs_buf_vc[job][2],
                               s->num_coeffs[job][2] >> 8, s->coeffs_buf_vc[job][3] - sizeof(int16_t) * s->num_coeffs[job][3],
                               s->num_coeffs[job][3] >> 10, 0, &s->coeffs_buf_accelerated[job]);
    //vpu_execute_code( vpu_get_fn(), vpu_get_constants(), s->coeffs_buf_vc[2], s->num_coeffs[2] >> 8, s->coeffs_buf_vc[3], s->num_coeffs[3] >> 10, 0);
    //gpu_cache_flush(&s->coeffs_buf_accelerated);
    //vpu_wait(s->vpu_id);

    for(i=0;i<4;i++)
        s->num_coeffs[job][i] = 0;
}
#endif


// I-pred, transform_and_add for all blocks types done here
// All ARM
#define RPI_OPT_SEP_PRED 0

#if RPI_OPT_SEP_PRED
static void rpi_execute_pred_cmds(HEVCContext * const s, const int do_luma, const int do_chroma)
#else
static void rpi_execute_pred_cmds(HEVCContext * const s)
#endif
{
  int i;
  HEVCRpiIntraPredEnv * iap = &s->jb1->intra;
  const HEVCPredCmd *cmd = iap->cmds;
#ifdef RPI_WORKER
  HEVCLocalContextIntra *lc = &s->HEVClcIntra;
#else
  HEVCLocalContext *lc = s->HEVClc;
#endif

  for(i = iap->n; i > 0; i--, cmd++) {
//      printf("i=%d cmd=%p job1=%d job0=%d\n",i,cmd,s->pass1_job,s->pass0_job);
#if RPI_OPT_SEP_PRED
      if (!(cmd->c_idx == 0 ? do_luma : do_chroma)) {
          continue;
      }
#endif

      switch (cmd->type)
      {
          case RPI_PRED_INTRA:
              lc->tu.intra_pred_mode_c = lc->tu.intra_pred_mode = cmd->i_pred.mode;
              lc->na.cand_bottom_left  = (cmd->na >> 4) & 1;
              lc->na.cand_left         = (cmd->na >> 3) & 1;
              lc->na.cand_up_left      = (cmd->na >> 2) & 1;
              lc->na.cand_up           = (cmd->na >> 1) & 1;
              lc->na.cand_up_right     = (cmd->na >> 0) & 1;
              if (!av_rpi_is_sand_frame(s->frame) || cmd->c_idx == 0)
                  s->hpc.intra_pred[cmd->size - 2](s, cmd->i_pred.x, cmd->i_pred.y, cmd->c_idx);
              else
                  s->hpc.intra_pred_c[cmd->size - 2](s, cmd->i_pred.x, cmd->i_pred.y, cmd->c_idx);
              break;

          case RPI_PRED_ADD_RESIDUAL:
              s->hevcdsp.add_residual[cmd->size - 2](cmd->ta.dst, (int16_t *)cmd->ta.buf, cmd->ta.stride);
              break;
#if RPI_HEVC_SAND
          case RPI_PRED_ADD_RESIDUAL_U:
              s->hevcdsp.add_residual_u[cmd->size - 2](cmd->ta.dst, (int16_t *)cmd->ta.buf, cmd->ta.stride);
              break;
          case RPI_PRED_ADD_RESIDUAL_V:
              s->hevcdsp.add_residual_v[cmd->size - 2](cmd->ta.dst, (int16_t *)cmd->ta.buf, cmd->ta.stride);
              break;
          case RPI_PRED_ADD_RESIDUAL_C:
              s->hevcdsp.add_residual_c[cmd->size - 2](cmd->ta.dst, (int16_t *)cmd->ta.buf, cmd->ta.stride);
              break;
#endif
          case RPI_PRED_I_PCM:
              pcm_extract(s, cmd->i_pcm.src, cmd->i_pcm.src_len, cmd->i_pcm.x, cmd->i_pcm.y, 1 << cmd->size);
              break;

          default:
              av_log(NULL, AV_LOG_PANIC, "Bad command %d in worker pred Q\n", cmd->type);
              abort();
      }
  }
#if RPI_OPT_SEP_PRED
  if (do_luma)
#endif
  {
      iap->n = 0;
  }
}


#endif

#ifdef RPI

// Set initial uniform job values & zero ctu_count
static void rpi_begin(HEVCContext *s)
{
#if RPI_INTER
    unsigned int i;
    HEVCRpiJob * const jb = s->jb0;
    HEVCRpiInterPredEnv *const cipe = &jb->chroma_ip;
    HEVCRpiInterPredEnv *const yipe = &jb->luma_ip;

    const uint16_t pic_width_y        = s->ps.sps->width;
    const uint16_t pic_height_y       = s->ps.sps->height;

    const uint16_t pic_width_c        = s->ps.sps->width >> s->ps.sps->hshift[1];
    const uint16_t pic_height_c       = s->ps.sps->height >> s->ps.sps->vshift[1];

    rpi_inter_pred_reset(cipe);
    for (i = 0; i < cipe->n; i++) {
        HEVCRpiInterPredQ * const cp = cipe->q + i;
        qpu_mc_pred_c_s_t * const u = &cp->qpu_mc_base->c.s;

        u->next_src1.x = 0;
        u->next_src1.y = 0;
        u->next_src1.base = 0;
        u->pic_cw = pic_width_c;
        u->pic_ch = pic_height_c;
        u->stride2 = av_rpi_sand_frame_stride2(s->frame);
        u->stride1 = s->frame->linesize[1];
        u->wdenom = s->sh.chroma_log2_weight_denom;
        cp->last_l0 = &u->next_src1;

        u->next_fn = 0;
        u->next_src2.x = 0;
        u->next_src2.y = 0;
        u->next_src2.base = 0;
        cp->last_l1 = &u->next_src2;

        *(qpu_mc_pred_c_s_t **)&cp->qpu_mc_curr = u + 1;
    }

    rpi_inter_pred_reset(yipe);
    for (i = 0; i < yipe->n; i++) {
        HEVCRpiInterPredQ * const yp = yipe->q + i;
        qpu_mc_pred_y_s_t * const y = &yp->qpu_mc_base->y.s;

        y->next_src1.x = 0;
        y->next_src1.y = 0;
        y->next_src1.base = 0;
        y->next_src2.x = 0;
        y->next_src2.y = 0;
        y->next_src2.base = 0;
        y->pic_h = pic_height_y;
        y->pic_w = pic_width_y;
        y->stride2 = av_rpi_sand_frame_stride2(s->frame);
        y->stride1 = s->frame->linesize[0];
        y->wdenom = s->sh.luma_log2_weight_denom;
        y->next_fn = 0;
        yp->last_l0 = &y->next_src1;
        yp->last_l1 = &y->next_src2;

        *(qpu_mc_pred_y_s_t **)&yp->qpu_mc_curr = y + 1;
    }

    s->last_y8_p = NULL;
    s->last_y8_l1 = NULL;
#endif
    s->ctu_count = 0;
}
#endif


#if RPI_INTER
#if !RPI_QPU_EMU_Y || !RPI_QPU_EMU_C
static unsigned int mc_terminate_add_qpu(HEVCContext * const s,
                                     const vpu_qpu_job_h vqj,
                                     rpi_cache_flush_env_t * const rfe,
                                     HEVCRpiInterPredEnv * const ipe)
{
    unsigned int i;
    uint32_t mail[QPU_N_MAX][QPU_MAIL_EL_VALS];
    if (!ipe->used) {
        return 0;
    }

    if (ipe->curr != 0) {
        rpi_inter_pred_sync(ipe);
    }

    // Add final commands to Q
    for(i = 0; i != ipe->n; ++i) {
        HEVCRpiInterPredQ * const yp = ipe->q + i;
        qpu_mc_src_t *const p0 = yp->last_l0;
        qpu_mc_src_t *const p1 = yp->last_l1;

        ((uint32_t *)yp->qpu_mc_curr)[-1] = yp->code_exit;

        // Need to set the srcs for L0 & L1 to something that can be (pointlessly) prefetched
        p0->x = MC_DUMMY_X;
        p0->y = MC_DUMMY_Y;
        p0->base = s->qpu_dummy_frame_qpu;
        p1->x = MC_DUMMY_X;
        p1->y = MC_DUMMY_Y;
        p1->base = s->qpu_dummy_frame_qpu;

        yp->last_l0 = NULL;
        yp->last_l1 = NULL;

        // Add to mailbox list
        mail[i][0] = ipe->gptr.vc + ((uint8_t *)yp->qpu_mc_base - ipe->gptr.arm);
        mail[i][1] = yp->code_setup;
    }

#if RPI_CACHE_UNIF_MVS
    rpi_cache_flush_add_gm_ptr(rfe, &ipe->gptr, RPI_CACHE_FLUSH_MODE_WB_INVALIDATE);
#endif
    vpu_qpu_job_add_qpu(vqj, ipe->n, (uint32_t *)mail);

    return 1;
}
#endif

#if RPI_QPU_EMU_Y || RPI_QPU_EMU_C
static unsigned int mc_terminate_add_emu(HEVCContext * const s,
                                     const vpu_qpu_job_h vqj,
                                     rpi_cache_flush_env_t * const rfe,
                                     HEVCRpiInterPredEnv * const ipe)
{
    unsigned int i;
    if (!ipe->used) {
        return 0;
    }

    if (ipe->curr != 0) {
        rpi_inter_pred_sync(ipe);
    }

    // Add final commands to Q
    for(i = 0; i != ipe->n; ++i) {
        HEVCRpiInterPredQ * const yp = ipe->q + i;
        qpu_mc_src_t *const p0 = yp->last_l0;
        qpu_mc_src_t *const p1 = yp->last_l1;

        ((uint32_t *)yp->qpu_mc_curr)[-1] = yp->code_exit;

        // Need to set the srcs for L0 & L1 to something that can be (pointlessly) prefetched
        p0->x = MC_DUMMY_X;
        p0->y = MC_DUMMY_Y;
        p0->base = s->qpu_dummy_frame_emu;
        p1->x = MC_DUMMY_X;
        p1->y = MC_DUMMY_Y;
        p1->base = s->qpu_dummy_frame_emu;

        yp->last_l0 = NULL;
        yp->last_l1 = NULL;
    }

    return 1;
}
#endif


#if RPI_QPU_EMU_Y
#define mc_terminate_add_y mc_terminate_add_emu
#else
#define mc_terminate_add_y mc_terminate_add_qpu
#endif
#if RPI_QPU_EMU_C
#define mc_terminate_add_c mc_terminate_add_emu
#else
#define mc_terminate_add_c mc_terminate_add_qpu
#endif
#endif

#ifdef RPI


static void flush_frame(HEVCContext *s,AVFrame *frame)
{
  rpi_cache_flush_env_t * rfe = rpi_cache_flush_init();
  rpi_cache_flush_add_frame(rfe, frame, RPI_CACHE_FLUSH_MODE_WB_INVALIDATE);
  rpi_cache_flush_finish(rfe);
}


// Core execution tasks
static void worker_core(HEVCContext * const s)
{
    threadlog_update(threadlog_reason_RUNNING_SETUP, +1);
#if RPI_OPT_SEP_PRED
    vpu_qpu_wait_h sync_c;
#endif
    vpu_qpu_wait_h sync_y;

    unsigned int flush_start = 0;
    unsigned int flush_count = 0;
    HEVCRpiJob * const jb = s->jb1;

    const vpu_qpu_job_h vqj = vpu_qpu_job_new();
    rpi_cache_flush_env_t * const rfe = rpi_cache_flush_init();

    {
        const HEVCRpiCoeffsEnv * const cf = &jb->coeffs;
        if (cf->s[3].n + cf->s[2].n != 0)
        {
            vpu_qpu_job_add_vpu(vqj,
                vpu_get_fn(s->ps.sps->bit_depth),
                vpu_get_constants(),
                cf->gptr.vc,
                cf->s[2].n >> 8,
                cf->gptr.vc + ((cf->s[3].buf - cf->s[2].buf) - cf->s[3].n) * sizeof(cf->s[3].buf[0]),
                cf->s[3].n >> 10,
                0);

            rpi_cache_flush_add_gm_ptr(rfe, &cf->gptr, RPI_CACHE_FLUSH_MODE_WB_INVALIDATE);
        }
    }

#if RPI_INTER
    {
        const HEVCRpiDeblkEnv *const de = &jb->deblk;
        unsigned int i;
        unsigned int high = de->blks[0].y_ctb;

        flush_start = high;
        for (i = 1; i < de->n; ++i)
        {
            const unsigned int y = de->blks[i].y_ctb;
            flush_start = FFMIN(flush_start, y);
            high = FFMAX(high, y);
        }
        flush_count = FFMIN(high + (1 << s->ps.sps->log2_ctb_size), s->ps.sps->height) - flush_start;
    }

    if (mc_terminate_add_c(s, vqj, rfe, &jb->chroma_ip) != 0)
    {
        rpi_cache_flush_add_frame_block(rfe, s->frame, RPI_CACHE_FLUSH_MODE_INVALIDATE,
          0, flush_start, s->ps.sps->width, flush_count, s->ps.sps->vshift[1], 0, 1);
    }

// We can take a sync here and try to locally overlap QPU processing with ARM
// but testing showed a slightly negative benefit with noticable extra complexity
#if RPI_OPT_SEP_PRED
    vpu_qpu_job_add_sync_this(vqj, &sync_c);
#endif

    if (mc_terminate_add_y(s, vqj, rfe, &jb->luma_ip) != 0)
    {
        rpi_cache_flush_add_frame_block(rfe, s->frame, RPI_CACHE_FLUSH_MODE_INVALIDATE,
          0, flush_start, s->ps.sps->width, flush_count, s->ps.sps->vshift[1], 1, 0);
    }
#endif

    vpu_qpu_job_add_sync_this(vqj, &sync_y);
    threadlog_update(threadlog_reason_RUNNING_SETUP, -1);

    // Having accumulated some commands - do them
    threadlog_update(threadlog_reason_RUNNING_FLUSH, +1);
    rpi_cache_flush_finish(rfe);
    threadlog_update(threadlog_reason_RUNNING_FLUSH, -1);
    vpu_qpu_job_finish(vqj);

    worker_pic_reset(&jb->coeffs);

    // We would do ARM inter prediction here but no longer
    // Look back in git if you find you want it back - As we have
    // no arm/neon sand pred code there doesn't seem a lot of point
    // keeping it around
#if RPI_QPU_EMU_Y || RPI_QPU_EMU_C
    if (av_rpi_is_sand8_frame(s->frame))
#if RPI_QPU_EMU_Y && RPI_QPU_EMU_C
        rpi_shader_c8(s, &jb->luma_ip, &jb->chroma_ip);
#elif RPI_QPU_EMU_Y
        rpi_shader_c8(s, &jb->luma_ip, NULL);
#else
        rpi_shader_c8(s, NULL, &jb->chroma_ip);
#endif
    else
#if RPI_QPU_EMU_Y && RPI_QPU_EMU_C
        rpi_shader_c16(s, &jb->luma_ip, &jb->chroma_ip);
#elif RPI_QPU_EMU_Y
        rpi_shader_c16(s, &jb->luma_ip, NULL);
#else
        rpi_shader_c16(s, NULL, &jb->chroma_ip);
#endif
#endif

#if RPI_OPT_SEP_PRED
    // Wait for transform completion
    vpu_qpu_wait(&sync_c);

    // Perform intra prediction and residual reconstruction
    rpi_execute_pred_cmds(s, 0, 1);

    // Wait for transform completion
    vpu_qpu_wait(&sync_y);

    // Perform intra prediction and residual reconstruction
    rpi_execute_pred_cmds(s, 1, 0);
#else
    // Wait for transform completion
    vpu_qpu_wait(&sync_y);

    // Perform intra prediction and residual reconstruction
    threadlog_update(threadlog_reason_RUNNING_INTRA_PRED, +1);
    rpi_execute_pred_cmds(s);
    threadlog_update(threadlog_reason_RUNNING_INTRA_PRED, -1);
#endif

    // Perform deblocking for CTBs in this row
    threadlog_update(threadlog_reason_RUNNING_DEBLOCK, +1);
    rpi_execute_dblk_cmds(s);
    threadlog_update(threadlog_reason_RUNNING_DEBLOCK, -1);
}

static void rpi_do_all_passes(HEVCContext *s)
{
    // Called from main thread - must be no pending background jobs
    av_assert0(s->pass0_job == s->pass1_job && s->jb0 == s->jb1 && !s->jb0->pending);

    // Do the various passes - common with the worker code
    worker_core(s);
    // Prepare next batch
    rpi_begin(s);
}


#endif

static int hls_decode_entry(AVCodecContext *avctxt, void *isFilterThread)
{
    HEVCContext *s  = avctxt->priv_data;
    int ctb_size    = 1 << s->ps.sps->log2_ctb_size;
    int more_data   = 1;
    int x_ctb       = 0;
    int y_ctb       = 0;
    int ctb_addr_ts = s->ps.pps->ctb_addr_rs_to_ts[s->sh.slice_ctb_addr_rs];

#ifdef RPI
    // * We don't support cross_component_prediction_enabled_flag but as that
    //   must be 0 unless we have 4:4:4 there is no point testing for it as we
    //   only deal with sand which is never 4:4:4
    //   [support wouldn't be hard]
    s->enable_rpi =
        ((s->ps.sps->bit_depth == 8 && s->frame->format == AV_PIX_FMT_SAND128) ||
         (s->ps.sps->bit_depth == 10 && s->frame->format == AV_PIX_FMT_SAND64_10));
#endif
    //printf("L0=%d L1=%d\n",s->sh.nb_refs[L1],s->sh.nb_refs[L1]);

    if (!ctb_addr_ts && s->sh.dependent_slice_segment_flag) {
        av_log(s->avctx, AV_LOG_ERROR, "Impossible initial tile.\n");
        return AVERROR_INVALIDDATA;
    }

    if (s->sh.dependent_slice_segment_flag) {
        int prev_rs = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts - 1];
        if (s->tab_slice_address[prev_rs] != s->sh.slice_addr) {
            av_log(s->avctx, AV_LOG_ERROR, "Previous slice segment missing\n");
            return AVERROR_INVALIDDATA;
        }
    }

#ifdef RPI_WORKER
    // Worker must be idle at start
    av_assert0(s->pass0_job == s->pass1_job && s->jb0 == s->jb1 && !s->jb0->pending);
#endif

#ifdef RPI
    rpi_begin(s);
#endif

    if (s->used_for_ref)
        threadlog_update(threadlog_reason_RUNNING_REF_CODING_TREE, +1);
    while (more_data && ctb_addr_ts < s->ps.sps->ctb_size) {
        int ctb_addr_rs = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts];

        x_ctb = (ctb_addr_rs % ((s->ps.sps->width + ctb_size - 1) >> s->ps.sps->log2_ctb_size)) << s->ps.sps->log2_ctb_size;
        y_ctb = (ctb_addr_rs / ((s->ps.sps->width + ctb_size - 1) >> s->ps.sps->log2_ctb_size)) << s->ps.sps->log2_ctb_size;
        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);


        ff_hevc_cabac_init(s, ctb_addr_ts);

        hls_sao_param(s, x_ctb >> s->ps.sps->log2_ctb_size, y_ctb >> s->ps.sps->log2_ctb_size);

        s->deblock[ctb_addr_rs].beta_offset = s->sh.beta_offset;
        s->deblock[ctb_addr_rs].tc_offset   = s->sh.tc_offset;
        s->filter_slice_edges[ctb_addr_rs]  = s->sh.slice_loop_filter_across_slices_enabled_flag;

        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->ps.sps->log2_ctb_size, 0);

        if (s->ref && s->threads_type == FF_THREAD_FRAME && y_ctb > ctb_size) {
            ff_thread_report_progress(&s->ref->tf, y_ctb - ctb_size, 1);
        }

#ifdef RPI
        if (s->enable_rpi) {
            int q_full = (s->ctu_count >= s->max_ctu_count);

            if (rpi_inter_pred_next_ctu(&s->jb0->luma_ip) != 0)
                q_full = 1;
            if (rpi_inter_pred_next_ctu(&s->jb0->chroma_ip) != 0)
                q_full = 1;

            s->jb0->deblk.blks[s->jb0->deblk.n].x_ctb = x_ctb;
            s->jb0->deblk.blks[s->jb0->deblk.n++].y_ctb = y_ctb;
            s->ctu_count++;

            if (q_full) {
#ifdef RPI_WORKER
                if (s->used_for_ref)
                {
//                  printf("%d %d/%d job=%d, x,y=%d,%d\n",s->ctu_count,s->num_dblk_cmds[s->pass0_job],RPI_MAX_DEBLOCK_CMDS,s->pass0_job, x_ctb, y_ctb);

//                  worker_wait(s);
                    // Split work load onto separate threads so we make as rapid progress as possible with this frame
                    // Pass on this job to worker thread
                    worker_submit_job(s);

                    // Make sure we have space to prepare the next job
                    worker_pass0_ready(s);

                    // Prepare the next batch of commands
                    rpi_begin(s);
                } else {
                    // Non-ref frame so do it all on this thread
                    rpi_do_all_passes(s);
                }
#else
                rpi_do_all_passes(s);
#endif
            }

        }
#endif


        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            return more_data;
        }


        ctb_addr_ts++;
        ff_hevc_save_states(s, ctb_addr_ts);
#ifdef RPI
        if (s->enable_rpi)
            continue;
#endif
        ff_hevc_hls_filters(s, x_ctb, y_ctb, ctb_size);
    }
    if (s->used_for_ref)
        threadlog_update(threadlog_reason_RUNNING_REF_CODING_TREE, -1);

#ifdef RPI

#ifdef RPI_WORKER
    // Wait for the worker to finish all its jobs
    if (s->enable_rpi) {
        worker_wait(s);
    }
#endif

    // Finish off any half-completed rows
    if (s->enable_rpi && s->ctu_count) {
        rpi_do_all_passes(s);
    }

#if RPI_TSTATS
    {
        HEVCRpiStats *const ts = &s->tstats;

        printf("=== P: xy00:%5d/%5d/%5d/%5d h16gl:%5d/%5d w8gl:%5d/%5d y8m:%d\n    B: xy00:%5d/%5d/%5d/%5d h16gl:%5d/%5d\n",
               ts->y_pred1_xy, ts->y_pred1_x0, ts->y_pred1_y0, ts->y_pred1_x0y0,
               ts->y_pred1_hgt16, ts->y_pred1_hle16, ts->y_pred1_wgt8, ts->y_pred1_wle8, ts->y_pred1_y8_merge,
               ts->y_pred2_xy, ts->y_pred2_x0, ts->y_pred2_y0, ts->y_pred2_x0y0,
               ts->y_pred2_hgt16, ts->y_pred2_hle16);
        memset(ts, 0, sizeof(*ts));
    }
#endif

#endif

    if (x_ctb + ctb_size >= s->ps.sps->width &&
        y_ctb + ctb_size >= s->ps.sps->height)
        ff_hevc_hls_filter(s, x_ctb, y_ctb, ctb_size);

    return ctb_addr_ts;
}

static int hls_slice_data(HEVCContext *s)
{
    int arg[2];
    int ret[2];

    arg[0] = 0;
    arg[1] = 1;

    s->avctx->execute(s->avctx, hls_decode_entry, arg, ret , 1, sizeof(int));
    return ret[0];
}
static int hls_decode_entry_wpp(AVCodecContext *avctxt, void *input_ctb_row, int job, int self_id)
{
    HEVCContext *s1  = avctxt->priv_data, *s;
    HEVCLocalContext *lc;
    int ctb_size    = 1<< s1->ps.sps->log2_ctb_size;
    int more_data   = 1;
    int *ctb_row_p    = input_ctb_row;
    int ctb_row = ctb_row_p[job];
    int ctb_addr_rs = s1->sh.slice_ctb_addr_rs + ctb_row * ((s1->ps.sps->width + ctb_size - 1) >> s1->ps.sps->log2_ctb_size);
    int ctb_addr_ts = s1->ps.pps->ctb_addr_rs_to_ts[ctb_addr_rs];
    int thread = ctb_row % s1->threads_number;
    int ret;

    s = s1->sList[self_id];
    lc = s->HEVClc;

#ifdef RPI
    s->enable_rpi = 0;
    //printf("Wavefront\n");
#endif

    if(ctb_row) {
        ret = init_get_bits8(&lc->gb, s->data + s->sh.offset[ctb_row - 1], s->sh.size[ctb_row - 1]);

        if (ret < 0)
            return ret;
        ff_init_cabac_decoder(&lc->cc, s->data + s->sh.offset[(ctb_row)-1], s->sh.size[ctb_row - 1]);
    }

    while(more_data && ctb_addr_ts < s->ps.sps->ctb_size) {
        int x_ctb = (ctb_addr_rs % s->ps.sps->ctb_width) << s->ps.sps->log2_ctb_size;
        int y_ctb = (ctb_addr_rs / s->ps.sps->ctb_width) << s->ps.sps->log2_ctb_size;

        hls_decode_neighbour(s, x_ctb, y_ctb, ctb_addr_ts);

        ff_thread_await_progress2(s->avctx, ctb_row, thread, SHIFT_CTB_WPP);

        if (atomic_load(&s1->wpp_err)) {
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return 0;
        }

        ff_hevc_cabac_init(s, ctb_addr_ts);
        hls_sao_param(s, x_ctb >> s->ps.sps->log2_ctb_size, y_ctb >> s->ps.sps->log2_ctb_size);
        more_data = hls_coding_quadtree(s, x_ctb, y_ctb, s->ps.sps->log2_ctb_size, 0);

        if (more_data < 0) {
            s->tab_slice_address[ctb_addr_rs] = -1;
            atomic_store(&s1->wpp_err, 1);
            ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);
            return more_data;
        }

        ctb_addr_ts++;

        ff_hevc_save_states(s, ctb_addr_ts);
        ff_thread_report_progress2(s->avctx, ctb_row, thread, 1);
        ff_hevc_hls_filters(s, x_ctb, y_ctb, ctb_size);

        if (!more_data && (x_ctb+ctb_size) < s->ps.sps->width && ctb_row != s->sh.num_entry_point_offsets) {
            atomic_store(&s1->wpp_err, 1);
            ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);
            return 0;
        }

        if ((x_ctb+ctb_size) >= s->ps.sps->width && (y_ctb+ctb_size) >= s->ps.sps->height ) {
            ff_hevc_hls_filter(s, x_ctb, y_ctb, ctb_size);
            ff_thread_report_progress2(s->avctx, ctb_row , thread, SHIFT_CTB_WPP);
            return ctb_addr_ts;
        }
        ctb_addr_rs       = s->ps.pps->ctb_addr_ts_to_rs[ctb_addr_ts];
        x_ctb+=ctb_size;

        if(x_ctb >= s->ps.sps->width) {
            break;
        }
    }
    ff_thread_report_progress2(s->avctx, ctb_row ,thread, SHIFT_CTB_WPP);

    return 0;
}

static int hls_slice_data_wpp(HEVCContext *s, const H2645NAL *nal)
{
    const uint8_t *data = nal->data;
    int length          = nal->size;
    HEVCLocalContext *lc = s->HEVClc;
    int *ret = av_malloc_array(s->sh.num_entry_point_offsets + 1, sizeof(int));
    int *arg = av_malloc_array(s->sh.num_entry_point_offsets + 1, sizeof(int));
    int64_t offset;
    int64_t startheader, cmpt = 0;
    int i, j, res = 0;

    if (!ret || !arg) {
        av_free(ret);
        av_free(arg);
        return AVERROR(ENOMEM);
    }

    if (s->sh.slice_ctb_addr_rs + s->sh.num_entry_point_offsets * s->ps.sps->ctb_width >= s->ps.sps->ctb_width * s->ps.sps->ctb_height) {
        av_log(s->avctx, AV_LOG_ERROR, "WPP ctb addresses are wrong (%d %d %d %d)\n",
            s->sh.slice_ctb_addr_rs, s->sh.num_entry_point_offsets,
            s->ps.sps->ctb_width, s->ps.sps->ctb_height
        );
        res = AVERROR_INVALIDDATA;
        goto error;
    }

    ff_alloc_entries(s->avctx, s->sh.num_entry_point_offsets + 1);

    if (!s->sList[1]) {
        for (i = 1; i < s->threads_number; i++) {
            s->sList[i] = av_malloc(sizeof(HEVCContext));
            memcpy(s->sList[i], s, sizeof(HEVCContext));
            s->HEVClcList[i] = av_mallocz(sizeof(HEVCLocalContext));
            s->sList[i]->HEVClc = s->HEVClcList[i];
        }
    }

    offset = (lc->gb.index >> 3);

    for (j = 0, cmpt = 0, startheader = offset + s->sh.entry_point_offset[0]; j < nal->skipped_bytes; j++) {
        if (nal->skipped_bytes_pos[j] >= offset && nal->skipped_bytes_pos[j] < startheader) {
            startheader--;
            cmpt++;
        }
    }

    for (i = 1; i < s->sh.num_entry_point_offsets; i++) {
        offset += (s->sh.entry_point_offset[i - 1] - cmpt);
        for (j = 0, cmpt = 0, startheader = offset
             + s->sh.entry_point_offset[i]; j < nal->skipped_bytes; j++) {
            if (nal->skipped_bytes_pos[j] >= offset && nal->skipped_bytes_pos[j] < startheader) {
                startheader--;
                cmpt++;
            }
        }
        s->sh.size[i - 1] = s->sh.entry_point_offset[i] - cmpt;
        s->sh.offset[i - 1] = offset;

    }
    if (s->sh.num_entry_point_offsets != 0) {
        offset += s->sh.entry_point_offset[s->sh.num_entry_point_offsets - 1] - cmpt;
        if (length < offset) {
            av_log(s->avctx, AV_LOG_ERROR, "entry_point_offset table is corrupted\n");
            res = AVERROR_INVALIDDATA;
            goto error;
        }
        s->sh.size[s->sh.num_entry_point_offsets - 1] = length - offset;
        s->sh.offset[s->sh.num_entry_point_offsets - 1] = offset;

    }
    s->data = data;

    for (i = 1; i < s->threads_number; i++) {
        s->sList[i]->HEVClc->first_qp_group = 1;
        s->sList[i]->HEVClc->qp_y = s->sList[0]->HEVClc->qp_y;
        memcpy(s->sList[i], s, sizeof(HEVCContext));
        s->sList[i]->HEVClc = s->HEVClcList[i];
    }

    atomic_store(&s->wpp_err, 0);
    ff_reset_entries(s->avctx);

    for (i = 0; i <= s->sh.num_entry_point_offsets; i++) {
        arg[i] = i;
        ret[i] = 0;
    }

    if (s->ps.pps->entropy_coding_sync_enabled_flag)
        s->avctx->execute2(s->avctx, hls_decode_entry_wpp, arg, ret, s->sh.num_entry_point_offsets + 1);

    for (i = 0; i <= s->sh.num_entry_point_offsets; i++)
        res += ret[i];
error:
    av_free(ret);
    av_free(arg);
    return res;
}

static int set_side_data(HEVCContext *s)
{
    AVFrame *out = s->ref->frame;

    if (s->sei_frame_packing_present &&
        s->frame_packing_arrangement_type >= 3 &&
        s->frame_packing_arrangement_type <= 5 &&
        s->content_interpretation_type > 0 &&
        s->content_interpretation_type < 3) {
        AVStereo3D *stereo = av_stereo3d_create_side_data(out);
        if (!stereo)
            return AVERROR(ENOMEM);

        switch (s->frame_packing_arrangement_type) {
        case 3:
            if (s->quincunx_subsampling)
                stereo->type = AV_STEREO3D_SIDEBYSIDE_QUINCUNX;
            else
                stereo->type = AV_STEREO3D_SIDEBYSIDE;
            break;
        case 4:
            stereo->type = AV_STEREO3D_TOPBOTTOM;
            break;
        case 5:
            stereo->type = AV_STEREO3D_FRAMESEQUENCE;
            break;
        }

        if (s->content_interpretation_type == 2)
            stereo->flags = AV_STEREO3D_FLAG_INVERT;
    }

    if (s->sei_display_orientation_present &&
        (s->sei_anticlockwise_rotation || s->sei_hflip || s->sei_vflip)) {
        double angle = s->sei_anticlockwise_rotation * 360 / (double) (1 << 16);
        AVFrameSideData *rotation = av_frame_new_side_data(out,
                                                           AV_FRAME_DATA_DISPLAYMATRIX,
                                                           sizeof(int32_t) * 9);
        if (!rotation)
            return AVERROR(ENOMEM);

        av_display_rotation_set((int32_t *)rotation->data, angle);
        av_display_matrix_flip((int32_t *)rotation->data,
                               s->sei_hflip, s->sei_vflip);
    }

    // Decrement the mastering display flag when IRAP frame has no_rasl_output_flag=1
    // so the side data persists for the entire coded video sequence.
    if (s->sei_mastering_display_info_present > 0 &&
        IS_IRAP(s) && s->no_rasl_output_flag) {
        s->sei_mastering_display_info_present--;
    }
    if (s->sei_mastering_display_info_present) {
        // HEVC uses a g,b,r ordering, which we convert to a more natural r,g,b
        const int mapping[3] = {2, 0, 1};
        const int chroma_den = 50000;
        const int luma_den = 10000;
        int i;
        AVMasteringDisplayMetadata *metadata =
            av_mastering_display_metadata_create_side_data(out);
        if (!metadata)
            return AVERROR(ENOMEM);

        for (i = 0; i < 3; i++) {
            const int j = mapping[i];
            metadata->display_primaries[i][0].num = s->display_primaries[j][0];
            metadata->display_primaries[i][0].den = chroma_den;
            metadata->display_primaries[i][1].num = s->display_primaries[j][1];
            metadata->display_primaries[i][1].den = chroma_den;
        }
        metadata->white_point[0].num = s->white_point[0];
        metadata->white_point[0].den = chroma_den;
        metadata->white_point[1].num = s->white_point[1];
        metadata->white_point[1].den = chroma_den;

        metadata->max_luminance.num = s->max_mastering_luminance;
        metadata->max_luminance.den = luma_den;
        metadata->min_luminance.num = s->min_mastering_luminance;
        metadata->min_luminance.den = luma_den;
        metadata->has_luminance = 1;
        metadata->has_primaries = 1;

        av_log(s->avctx, AV_LOG_DEBUG, "Mastering Display Metadata:\n");
        av_log(s->avctx, AV_LOG_DEBUG,
               "r(%5.4f,%5.4f) g(%5.4f,%5.4f) b(%5.4f %5.4f) wp(%5.4f, %5.4f)\n",
               av_q2d(metadata->display_primaries[0][0]),
               av_q2d(metadata->display_primaries[0][1]),
               av_q2d(metadata->display_primaries[1][0]),
               av_q2d(metadata->display_primaries[1][1]),
               av_q2d(metadata->display_primaries[2][0]),
               av_q2d(metadata->display_primaries[2][1]),
               av_q2d(metadata->white_point[0]), av_q2d(metadata->white_point[1]));
        av_log(s->avctx, AV_LOG_DEBUG,
               "min_luminance=%f, max_luminance=%f\n",
               av_q2d(metadata->min_luminance), av_q2d(metadata->max_luminance));
    }

    if (s->a53_caption) {
        AVFrameSideData* sd = av_frame_new_side_data(out,
                                                     AV_FRAME_DATA_A53_CC,
                                                     s->a53_caption_size);
        if (sd)
            memcpy(sd->data, s->a53_caption, s->a53_caption_size);
        av_freep(&s->a53_caption);
        s->a53_caption_size = 0;
        s->avctx->properties |= FF_CODEC_PROPERTY_CLOSED_CAPTIONS;
    }

    return 0;
}

static int hevc_frame_start(HEVCContext *s)
{
    HEVCLocalContext *lc = s->HEVClc;
    int pic_size_in_ctb  = ((s->ps.sps->width  >> s->ps.sps->log2_min_cb_size) + 1) *
                           ((s->ps.sps->height >> s->ps.sps->log2_min_cb_size) + 1);
    int ret;

    memset(s->horizontal_bs, 0, s->bs_width * s->bs_height);
    memset(s->vertical_bs,   0, s->bs_width * s->bs_height);
    memset(s->cbf_luma,      0, s->ps.sps->min_tb_width * s->ps.sps->min_tb_height);
    memset(s->is_pcm,        0, (s->ps.sps->min_pu_width + 1) * (s->ps.sps->min_pu_height + 1));
    memset(s->tab_slice_address, -1, pic_size_in_ctb * sizeof(*s->tab_slice_address));

    s->is_decoded        = 0;
    s->first_nal_type    = s->nal_unit_type;

    s->no_rasl_output_flag = IS_IDR(s) || IS_BLA(s) || (s->nal_unit_type == HEVC_NAL_CRA_NUT && s->last_eos);

    if (s->ps.pps->tiles_enabled_flag)
        lc->end_of_tiles_x = s->ps.pps->column_width[0] << s->ps.sps->log2_ctb_size;

    ret = ff_hevc_set_new_ref(s, &s->frame, s->poc);
    if (ret < 0)
        goto fail;

    ret = ff_hevc_frame_rps(s);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR, "Error constructing the frame RPS.\n");
        goto fail;
    }

    s->ref->frame->key_frame = IS_IRAP(s);

    ret = set_side_data(s);
    if (ret < 0)
        goto fail;

    s->frame->pict_type = 3 - s->sh.slice_type;

    if (!IS_IRAP(s))
        ff_hevc_bump_frame(s);

    av_frame_unref(s->output_frame);
    ret = ff_hevc_output_frame(s, s->output_frame, 0);
    if (ret < 0)
        goto fail;

    if (!s->avctx->hwaccel)
        ff_thread_finish_setup(s->avctx);

    return 0;

fail:
    if (s->ref)
        ff_hevc_unref_frame(s, s->ref, ~0);
    s->ref = NULL;
    return ret;
}

static int decode_nal_unit(HEVCContext *s, const H2645NAL *nal)
{
    HEVCLocalContext *lc = s->HEVClc;
    GetBitContext *gb    = &lc->gb;
    int ctb_addr_ts, ret;

    *gb              = nal->gb;
    s->nal_unit_type = nal->type;
    s->temporal_id   = nal->temporal_id;

    switch (s->nal_unit_type) {
    case HEVC_NAL_VPS:
        ret = ff_hevc_decode_nal_vps(gb, s->avctx, &s->ps);
        if (ret < 0)
            goto fail;
        break;
    case HEVC_NAL_SPS:
        ret = ff_hevc_decode_nal_sps(gb, s->avctx, &s->ps,
                                     s->apply_defdispwin);
        if (ret < 0)
            goto fail;
        break;
    case HEVC_NAL_PPS:
        ret = ff_hevc_decode_nal_pps(gb, s->avctx, &s->ps);
        if (ret < 0)
            goto fail;
        break;
    case HEVC_NAL_SEI_PREFIX:
    case HEVC_NAL_SEI_SUFFIX:
        ret = ff_hevc_decode_nal_sei(s);
        if (ret < 0)
            goto fail;
        break;
    case HEVC_NAL_TRAIL_R:
    case HEVC_NAL_TRAIL_N:
    case HEVC_NAL_TSA_N:
    case HEVC_NAL_TSA_R:
    case HEVC_NAL_STSA_N:
    case HEVC_NAL_STSA_R:
    case HEVC_NAL_BLA_W_LP:
    case HEVC_NAL_BLA_W_RADL:
    case HEVC_NAL_BLA_N_LP:
    case HEVC_NAL_IDR_W_RADL:
    case HEVC_NAL_IDR_N_LP:
    case HEVC_NAL_CRA_NUT:
    case HEVC_NAL_RADL_N:
    case HEVC_NAL_RADL_R:
    case HEVC_NAL_RASL_N:
    case HEVC_NAL_RASL_R:
        ret = hls_slice_header(s);
        if (ret < 0)
            return ret;

        // The definition of _N unit types is "non-reference for other frames
        // with the same temporal_id" so they may/will be ref frames for pics
        // with a higher temporal_id.
        s->used_for_ref = s->ps.sps->max_sub_layers > s->temporal_id + 1 ||
            !(s->nal_unit_type == HEVC_NAL_TRAIL_N ||
                        s->nal_unit_type == HEVC_NAL_TSA_N   ||
                        s->nal_unit_type == HEVC_NAL_STSA_N  ||
                        s->nal_unit_type == HEVC_NAL_RADL_N  ||
                        s->nal_unit_type == HEVC_NAL_RASL_N);
        s->avctx->internal->sched_priority_offset = s->used_for_ref * 32;
        ff_thread_apply_sched_priority(s->avctx);

#if DEBUG_DECODE_N
        {
            static int z = 0;
            if (IS_IDR(s)) {
                z = 1;
            }
            if (z != 0 && z++ > DEBUG_DECODE_N) {
                s->is_decoded = 0;
                break;
            }
        }
#endif
        if (!s->used_for_ref && s->avctx->skip_frame >= AVDISCARD_NONREF) {
            s->is_decoded = 0;
            break;
        }

        if (s->sh.first_slice_in_pic_flag) {
            if (s->max_ra == INT_MAX) {
                if (s->nal_unit_type == HEVC_NAL_CRA_NUT || IS_BLA(s)) {
                    s->max_ra = s->poc;
                } else {
                    if (IS_IDR(s))
                        s->max_ra = INT_MIN;
                }
            }

            if ((s->nal_unit_type == HEVC_NAL_RASL_R || s->nal_unit_type == HEVC_NAL_RASL_N) &&
                s->poc <= s->max_ra) {
                s->is_decoded = 0;
                break;
            } else {
                if (s->nal_unit_type == HEVC_NAL_RASL_R && s->poc > s->max_ra)
                    s->max_ra = INT_MIN;
            }

            ret = hevc_frame_start(s);
            if (ret < 0)
                return ret;
        } else if (!s->ref) {
            av_log(s->avctx, AV_LOG_ERROR, "First slice in a frame missing.\n");
            goto fail;
        }

        if (s->nal_unit_type != s->first_nal_type) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "Non-matching NAL types of the VCL NALUs: %d %d\n",
                   s->first_nal_type, s->nal_unit_type);
            return AVERROR_INVALIDDATA;
        }

        if (!s->sh.dependent_slice_segment_flag &&
            s->sh.slice_type != HEVC_SLICE_I) {
            ret = ff_hevc_slice_rpl(s);
            if (ret < 0) {
                av_log(s->avctx, AV_LOG_WARNING,
                       "Error constructing the reference lists for the current slice.\n");
                goto fail;
            }
        }

        if (s->sh.first_slice_in_pic_flag && s->avctx->hwaccel) {
            ret = s->avctx->hwaccel->start_frame(s->avctx, NULL, 0);
            if (ret < 0)
                goto fail;
        }

        if (s->avctx->hwaccel) {
            ret = s->avctx->hwaccel->decode_slice(s->avctx, nal->raw_data, nal->raw_size);
            if (ret < 0)
                goto fail;
        } else {
            if (s->threads_number > 1 && s->sh.num_entry_point_offsets > 0)
                ctb_addr_ts = hls_slice_data_wpp(s, nal);
            else
                ctb_addr_ts = hls_slice_data(s);
            if (ctb_addr_ts >= (s->ps.sps->ctb_width * s->ps.sps->ctb_height)) {
                s->is_decoded = 1;
            }

            if (ctb_addr_ts < 0) {
                ret = ctb_addr_ts;
                goto fail;
            }
        }
        break;
    case HEVC_NAL_EOS_NUT:
    case HEVC_NAL_EOB_NUT:
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra     = INT_MAX;
        break;
    case HEVC_NAL_AUD:
    case HEVC_NAL_FD_NUT:
        break;
    default:
        av_log(s->avctx, AV_LOG_INFO,
               "Skipping NAL unit %d\n", s->nal_unit_type);
    }

    return 0;
fail:
    if (s->avctx->err_recognition & AV_EF_EXPLODE)
        return ret;
    return 0;
}

static int decode_nal_units(HEVCContext *s, const uint8_t *buf, int length)
{
    int i, ret = 0;

    s->ref = NULL;
    s->last_eos = s->eos;
    s->eos = 0;

    /* split the input packet into NAL units, so we know the upper bound on the
     * number of slices in the frame */
    ret = ff_h2645_packet_split(&s->pkt, buf, length, s->avctx, s->is_nalff,
                                s->nal_length_size, s->avctx->codec_id, 1);
    if (ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR,
               "Error splitting the input into NAL units.\n");
        return ret;
    }

    for (i = 0; i < s->pkt.nb_nals; i++) {
        if (s->pkt.nals[i].type == HEVC_NAL_EOB_NUT ||
            s->pkt.nals[i].type == HEVC_NAL_EOS_NUT)
            s->eos = 1;
    }

    /* decode the NAL units */
    for (i = 0; i < s->pkt.nb_nals; i++) {
        ret = decode_nal_unit(s, &s->pkt.nals[i]);
        if (ret < 0) {
            av_log(s->avctx, AV_LOG_WARNING,
                   "Error parsing NAL unit #%d.\n", i);
            goto fail;
        }
    }

fail:  // Also success path
    if (s->ref && s->threads_type == FF_THREAD_FRAME) {
#if RPI_INTER
        rpi_flush_ref_frame_progress(s, &s->ref->tf, s->ps.sps->height);
#endif
        ff_thread_report_progress(&s->ref->tf, INT_MAX, 0);
        ff_thread_report_progress(&s->ref->tf, INT_MAX, 1);
    }
#if RPI_INTER
    else if (s->ref && s->enable_rpi) {
      // When running single threaded we need to flush the whole frame
      flush_frame(s,s->frame);
    }
#endif
    return ret;
}

static void print_md5(void *log_ctx, int level, uint8_t md5[16])
{
    int i;
    for (i = 0; i < 16; i++)
        av_log(log_ctx, level, "%02"PRIx8, md5[i]);
}

static int verify_md5(HEVCContext *s, AVFrame *frame)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    int pixel_shift;
    int i, j;

    if (!desc)
        return AVERROR(EINVAL);

    pixel_shift = desc->comp[0].depth > 8;

    av_log(s->avctx, AV_LOG_DEBUG, "Verifying checksum for frame with POC %d: ",
           s->poc);

    /* the checksums are LE, so we have to byteswap for >8bpp formats
     * on BE arches */
#if HAVE_BIGENDIAN
    if (pixel_shift && !s->checksum_buf) {
        av_fast_malloc(&s->checksum_buf, &s->checksum_buf_size,
                       FFMAX3(frame->linesize[0], frame->linesize[1],
                              frame->linesize[2]));
        if (!s->checksum_buf)
            return AVERROR(ENOMEM);
    }
#endif

    for (i = 0; frame->data[i]; i++) {
        int width  = s->avctx->coded_width;
        int height = s->avctx->coded_height;
        int w = (i == 1 || i == 2) ? (width  >> desc->log2_chroma_w) : width;
        int h = (i == 1 || i == 2) ? (height >> desc->log2_chroma_h) : height;
        uint8_t md5[16];

        av_md5_init(s->md5_ctx);
        for (j = 0; j < h; j++) {
            const uint8_t *src = frame->data[i] + j * frame->linesize[i];
#if HAVE_BIGENDIAN
            if (pixel_shift) {
                s->bdsp.bswap16_buf((uint16_t *) s->checksum_buf,
                                    (const uint16_t *) src, w);
                src = s->checksum_buf;
            }
#endif
            av_md5_update(s->md5_ctx, src, w << pixel_shift);
        }
        av_md5_final(s->md5_ctx, md5);

        if (!memcmp(md5, s->md5[i], 16)) {
            av_log   (s->avctx, AV_LOG_DEBUG, "plane %d - correct ", i);
            print_md5(s->avctx, AV_LOG_DEBUG, md5);
            av_log   (s->avctx, AV_LOG_DEBUG, "; ");
        } else {
            av_log   (s->avctx, AV_LOG_ERROR, "mismatching checksum of plane %d - ", i);
            print_md5(s->avctx, AV_LOG_ERROR, md5);
            av_log   (s->avctx, AV_LOG_ERROR, " != ");
            print_md5(s->avctx, AV_LOG_ERROR, s->md5[i]);
            av_log   (s->avctx, AV_LOG_ERROR, "\n");
            return AVERROR_INVALIDDATA;
        }
    }

    av_log(s->avctx, AV_LOG_DEBUG, "\n");

    return 0;
}

static int hevc_decode_extradata(HEVCContext *s, uint8_t *buf, int length)
{
    AVCodecContext *avctx = s->avctx;
    GetByteContext gb;
    int ret, i;

    bytestream2_init(&gb, buf, length);

    if (length > 3 && (buf[0] || buf[1] || buf[2] > 1)) {
        /* It seems the extradata is encoded as hvcC format.
         * Temporarily, we support configurationVersion==0 until 14496-15 3rd
         * is finalized. When finalized, configurationVersion will be 1 and we
         * can recognize hvcC by checking if avctx->extradata[0]==1 or not. */
        int i, j, num_arrays, nal_len_size;

        s->is_nalff = 1;

        bytestream2_skip(&gb, 21);
        nal_len_size = (bytestream2_get_byte(&gb) & 3) + 1;
        num_arrays   = bytestream2_get_byte(&gb);

        /* nal units in the hvcC always have length coded with 2 bytes,
         * so put a fake nal_length_size = 2 while parsing them */
        s->nal_length_size = 2;

        /* Decode nal units from hvcC. */
        for (i = 0; i < num_arrays; i++) {
            int type = bytestream2_get_byte(&gb) & 0x3f;
            int cnt  = bytestream2_get_be16(&gb);

            for (j = 0; j < cnt; j++) {
                // +2 for the nal size field
                int nalsize = bytestream2_peek_be16(&gb) + 2;
                if (bytestream2_get_bytes_left(&gb) < nalsize) {
                    av_log(s->avctx, AV_LOG_ERROR,
                           "Invalid NAL unit size in extradata.\n");
                    return AVERROR_INVALIDDATA;
                }

                ret = decode_nal_units(s, gb.buffer, nalsize);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR,
                           "Decoding nal unit %d %d from hvcC failed\n",
                           type, i);
                    return ret;
                }
                bytestream2_skip(&gb, nalsize);
            }
        }

        /* Now store right nal length size, that will be used to parse
         * all other nals */
        s->nal_length_size = nal_len_size;
    } else {
        s->is_nalff = 0;
        ret = decode_nal_units(s, buf, length);
        if (ret < 0)
            return ret;
    }

    /* export stream parameters from the first SPS */
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.sps_list); i++) {
        if (s->ps.sps_list[i]) {
            const HEVCSPS *sps = (const HEVCSPS*)s->ps.sps_list[i]->data;
            export_stream_params(s->avctx, &s->ps, sps);
            break;
        }
    }

    return 0;
}

static int hevc_decode_frame(AVCodecContext *avctx, void *data, int *got_output,
                             AVPacket *avpkt)
{
    int ret;
    int new_extradata_size;
    uint8_t *new_extradata;
    HEVCContext *s = avctx->priv_data;

    if (!avpkt->size) {
        ret = ff_hevc_output_frame(s, data, 1);
        if (ret < 0)
            return ret;

        *got_output = ret;
        return 0;
    }

    new_extradata = av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA,
                                            &new_extradata_size);
    if (new_extradata && new_extradata_size > 0) {
        ret = hevc_decode_extradata(s, new_extradata, new_extradata_size);
        if (ret < 0)
            return ret;
    }

    s->ref = NULL;
    ret    = decode_nal_units(s, avpkt->data, avpkt->size);
    if (ret < 0)
        return ret;

    if (avctx->hwaccel) {
        if (s->ref && (ret = avctx->hwaccel->end_frame(avctx)) < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "hardware accelerator failed to decode picture\n");
            ff_hevc_unref_frame(s, s->ref, ~0);
            return ret;
        }
    } else {
        /* verify the SEI checksum */
        if (avctx->err_recognition & AV_EF_CRCCHECK && s->is_decoded &&
            s->is_md5) {
            ret = verify_md5(s, s->ref->frame);
            if (ret < 0 && avctx->err_recognition & AV_EF_EXPLODE) {
                ff_hevc_unref_frame(s, s->ref, ~0);
                return ret;
            }
        }
    }
    s->is_md5 = 0;

    if (s->is_decoded) {
        av_log(avctx, AV_LOG_DEBUG, "Decoded frame with POC %d.\n", s->poc);
        s->is_decoded = 0;
    }

    if (s->output_frame->buf[0]) {
        av_frame_move_ref(data, s->output_frame);
        *got_output = 1;
    }

    return avpkt->size;
}

static int hevc_ref_frame(HEVCContext *s, HEVCFrame *dst, HEVCFrame *src)
{
    int ret;

    ret = ff_thread_ref_frame(&dst->tf, &src->tf);
    if (ret < 0)
        return ret;

    dst->tab_mvf_buf = av_buffer_ref(src->tab_mvf_buf);
    if (!dst->tab_mvf_buf)
        goto fail;
    dst->tab_mvf = src->tab_mvf;

    dst->rpl_tab_buf = av_buffer_ref(src->rpl_tab_buf);
    if (!dst->rpl_tab_buf)
        goto fail;
    dst->rpl_tab = src->rpl_tab;

    dst->rpl_buf = av_buffer_ref(src->rpl_buf);
    if (!dst->rpl_buf)
        goto fail;

    dst->poc        = src->poc;
    dst->ctb_count  = src->ctb_count;
    dst->window     = src->window;
    dst->flags      = src->flags;
    dst->sequence   = src->sequence;

    if (src->hwaccel_picture_private) {
        dst->hwaccel_priv_buf = av_buffer_ref(src->hwaccel_priv_buf);
        if (!dst->hwaccel_priv_buf)
            goto fail;
        dst->hwaccel_picture_private = dst->hwaccel_priv_buf->data;
    }

    return 0;
fail:
    ff_hevc_unref_frame(s, dst, ~0);
    return AVERROR(ENOMEM);
}

#ifdef RPI_WORKER
static av_cold void hevc_init_worker(HEVCContext * const s)
{
    int err;

    memset(s->jobs, 0, sizeof(s->jobs));

    for (unsigned int job = 0; job < RPI_MAX_JOBS; job++) {
        HEVCRpiJob * const jb = s->jobs + job;

        sem_init(&jb->sem_in, 0, 0);
        sem_init(&jb->sem_out, 0, 0);

        jb->intra.n = 0;
        jb->intra.cmds = av_mallocz(sizeof(HEVCPredCmd) * RPI_MAX_PRED_CMDS);

        // ** Sizeof the union structure might be overkill but at the moment it
        //    is correct (it certainly isn't going to be too small)

        rpi_inter_pred_alloc(&jb->chroma_ip,
                             QPU_N_MAX, QPU_N_GRP,
                             QPU_C_COMMANDS * sizeof(qpu_mc_pred_c_t),
                             QPU_C_CMD_PER_CTU_MAX * sizeof(qpu_mc_pred_c_t));
        rpi_inter_pred_alloc(&jb->luma_ip,
                             QPU_N_MAX,  QPU_N_GRP,
                             QPU_Y_COMMANDS * sizeof(qpu_mc_pred_y_t),
                             QPU_Y_CMD_PER_CTU_MAX * sizeof(qpu_mc_pred_y_t));

        jb->deblk.n = 0;
        jb->deblk.blks = av_malloc(sizeof(jb->deblk.blks[0]) * RPI_MAX_DEBLOCK_CMDS);
    }
    s->pass0_job = 0;
    s->pass1_job = 0;
    s->jb0 = s->jobs + 0;
    s->jb1 = s->jobs + 0;

    err = pthread_create(&s->worker_thread, NULL, worker_start, s);
    if (err) {
        printf("Failed to create worker thread\n");
        exit(-1);
    }
}

static void rpi_free_inter_pred(HEVCRpiInterPredEnv * const ipe)
{
    av_freep(&ipe->q);
    gpu_free(&ipe->gptr);
}

static av_cold void hevc_exit_worker(HEVCContext *s)
{
    void *res;
    unsigned int i;

    for(i = 0; i < RPI_MAX_JOBS; i++)
        s->jobs[i].terminate = 1;
    for(i = 0; i < RPI_MAX_JOBS; i++)
        sem_post(&s->jobs[i].sem_in);
    pthread_join(s->worker_thread, &res);

    for(i = 0; i < RPI_MAX_JOBS; i++)
    {
        HEVCRpiJob * const jb = s->jobs + i;

        sem_destroy(&jb->sem_in);
        sem_destroy(&jb->sem_out);
        av_freep(&jb->intra.cmds);
        av_freep(&jb->deblk.blks);
        rpi_free_inter_pred(&jb->chroma_ip);
        rpi_free_inter_pred(&jb->luma_ip);
    }
}

#endif

static av_cold int hevc_decode_free(AVCodecContext *avctx)
{
    HEVCContext       *s = avctx->priv_data;
    int i;

    pic_arrays_free(s);

    av_freep(&s->md5_ctx);

    av_freep(&s->cabac_state);

#ifdef RPI

#ifdef RPI_WORKER
    hevc_exit_worker(s);
#endif

    vpu_qpu_term();

    av_rpi_zc_uninit(avctx);
#endif

    av_freep(&s->sao_pixel_buffer_h[0]);  // [1] & [2] allocated with [0]
    av_freep(&s->sao_pixel_buffer_v[0]);
    av_frame_free(&s->output_frame);

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        ff_hevc_unref_frame(s, &s->DPB[i], ~0);
        av_frame_free(&s->DPB[i].frame);
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.vps_list); i++)
        av_buffer_unref(&s->ps.vps_list[i]);
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.sps_list); i++)
        av_buffer_unref(&s->ps.sps_list[i]);
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.pps_list); i++)
        av_buffer_unref(&s->ps.pps_list[i]);
    s->ps.sps = NULL;
    s->ps.pps = NULL;
    s->ps.vps = NULL;

    av_freep(&s->sh.entry_point_offset);
    av_freep(&s->sh.offset);
    av_freep(&s->sh.size);

    for (i = 1; i < s->threads_number; i++) {
        HEVCLocalContext *lc = s->HEVClcList[i];
        if (lc) {
            av_freep(&s->HEVClcList[i]);
            av_freep(&s->sList[i]);
        }
    }
    if (s->HEVClc == s->HEVClcList[0])
        s->HEVClc = NULL;
    av_freep(&s->HEVClcList[0]);

    ff_h2645_packet_uninit(&s->pkt);

    return 0;
}


static av_cold int hevc_init_context(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int i;

    s->avctx = avctx;

    s->HEVClc = av_mallocz(sizeof(HEVCLocalContext));
    if (!s->HEVClc)
        goto fail;
    s->HEVClcList[0] = s->HEVClc;
    s->sList[0] = s;

#ifdef RPI
    // Whilst FFmpegs init fn is only called once the close fn is called as
    // many times as we have threads (init_thread_copy is called for the
    // threads).  So to match init & term put the init here where it will be
    // called by both init & copy
    av_rpi_zc_init(avctx);

    if (vpu_qpu_init() != 0)
        goto fail;

#if RPI_INTER
#if RPI_QPU_EMU_Y || RPI_QPU_EMU_C
    {
        static const uint32_t dframe[1] = {0x80808080};
        s->qpu_dummy_frame_emu = (const uint8_t *)dframe;
    }
#endif
#if !RPI_QPU_EMU_Y || !RPI_QPU_EMU_C
    s->qpu_dummy_frame_qpu = qpu_fn(mc_start);  // Use our code as a dummy frame
#endif
#endif
    //gpu_malloc_uncached(2048*64,&s->dummy);

    s->enable_rpi = 0;

#ifdef RPI_WORKER
    hevc_init_worker(s);
#endif

#endif

    s->cabac_state = av_malloc(HEVC_CONTEXTS);
    if (!s->cabac_state)
        goto fail;

    s->output_frame = av_frame_alloc();
    if (!s->output_frame)
        goto fail;

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        s->DPB[i].frame = av_frame_alloc();
        if (!s->DPB[i].frame)
            goto fail;
        s->DPB[i].tf.f = s->DPB[i].frame;
    }

    s->max_ra = INT_MAX;

    s->md5_ctx = av_md5_alloc();
    if (!s->md5_ctx)
        goto fail;

    ff_bswapdsp_init(&s->bdsp);

    s->context_initialized = 1;
    s->eos = 0;

    ff_hevc_reset_sei(s);

    return 0;

fail:
    hevc_decode_free(avctx);
    return AVERROR(ENOMEM);
}

static int hevc_update_thread_context(AVCodecContext *dst,
                                      const AVCodecContext *src)
{
    HEVCContext *s  = dst->priv_data;
    HEVCContext *s0 = src->priv_data;
    int i, ret;

    if (!s->context_initialized) {
        ret = hevc_init_context(dst);
        if (ret < 0)
            return ret;
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->DPB); i++) {
        ff_hevc_unref_frame(s, &s->DPB[i], ~0);
        if (s0->DPB[i].frame->buf[0]) {
            ret = hevc_ref_frame(s, &s->DPB[i], &s0->DPB[i]);
            if (ret < 0)
                return ret;
        }
    }

    if (s->ps.sps != s0->ps.sps)
        s->ps.sps = NULL;
    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.vps_list); i++) {
        av_buffer_unref(&s->ps.vps_list[i]);
        if (s0->ps.vps_list[i]) {
            s->ps.vps_list[i] = av_buffer_ref(s0->ps.vps_list[i]);
            if (!s->ps.vps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.sps_list); i++) {
        av_buffer_unref(&s->ps.sps_list[i]);
        if (s0->ps.sps_list[i]) {
            s->ps.sps_list[i] = av_buffer_ref(s0->ps.sps_list[i]);
            if (!s->ps.sps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    for (i = 0; i < FF_ARRAY_ELEMS(s->ps.pps_list); i++) {
        av_buffer_unref(&s->ps.pps_list[i]);
        if (s0->ps.pps_list[i]) {
            s->ps.pps_list[i] = av_buffer_ref(s0->ps.pps_list[i]);
            if (!s->ps.pps_list[i])
                return AVERROR(ENOMEM);
        }
    }

    if (s->ps.sps != s0->ps.sps)
        if ((ret = set_sps(s, s0->ps.sps, src->pix_fmt)) < 0)
            return ret;

    s->seq_decode = s0->seq_decode;
    s->seq_output = s0->seq_output;
    s->pocTid0    = s0->pocTid0;
    s->max_ra     = s0->max_ra;
    s->eos        = s0->eos;
    s->no_rasl_output_flag = s0->no_rasl_output_flag;

    s->is_nalff        = s0->is_nalff;
    s->nal_length_size = s0->nal_length_size;

    s->threads_number      = s0->threads_number;
    s->threads_type        = s0->threads_type;

    if (s0->eos) {
        s->seq_decode = (s->seq_decode + 1) & 0xff;
        s->max_ra = INT_MAX;
    }

    return 0;
}

static av_cold int hevc_decode_init(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int ret;

    avctx->internal->allocate_progress = 1;

    ret = hevc_init_context(avctx);
    if (ret < 0)
        return ret;

    s->enable_parallel_tiles = 0;
    s->picture_struct = 0;
    s->eos = 1;

    atomic_init(&s->wpp_err, 0);

    if(avctx->active_thread_type & FF_THREAD_SLICE)
        s->threads_number = avctx->thread_count;
    else
        s->threads_number = 1;

    if (avctx->extradata_size > 0 && avctx->extradata) {
        ret = hevc_decode_extradata(s, avctx->extradata, avctx->extradata_size);
        if (ret < 0) {
            hevc_decode_free(avctx);
            return ret;
        }
    }

    if((avctx->active_thread_type & FF_THREAD_FRAME) && avctx->thread_count > 1)
        s->threads_type = FF_THREAD_FRAME;
    else
        s->threads_type = FF_THREAD_SLICE;

    return 0;
}

static av_cold int hevc_init_thread_copy(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    int ret;

    memset(s, 0, sizeof(*s));

    ret = hevc_init_context(avctx);
    if (ret < 0)
        return ret;

    return 0;
}

static void hevc_decode_flush(AVCodecContext *avctx)
{
    HEVCContext *s = avctx->priv_data;
    ff_hevc_flush_dpb(s);
    s->max_ra = INT_MAX;
    s->eos = 1;
}

#define OFFSET(x) offsetof(HEVCContext, x)
#define PAR (AV_OPT_FLAG_DECODING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption options[] = {
    { "apply_defdispwin", "Apply default display window from VUI", OFFSET(apply_defdispwin),
        AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, PAR },
    { "strict-displaywin", "stricly apply default display window size", OFFSET(apply_defdispwin),
        AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, PAR },
    { NULL },
};

static const AVClass hevc_decoder_class = {
    .class_name = "HEVC decoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_decoder = {
    .name                  = "hevc",
    .long_name             = NULL_IF_CONFIG_SMALL("HEVC (High Efficiency Video Coding)"),
    .type                  = AVMEDIA_TYPE_VIDEO,
    .id                    = AV_CODEC_ID_HEVC,
    .priv_data_size        = sizeof(HEVCContext),
    .priv_class            = &hevc_decoder_class,
    .init                  = hevc_decode_init,
    .close                 = hevc_decode_free,
    .decode                = hevc_decode_frame,
    .flush                 = hevc_decode_flush,
    .update_thread_context = hevc_update_thread_context,
    .init_thread_copy      = hevc_init_thread_copy,
    .capabilities          = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_DELAY |
//                             0,
//                             AV_CODEC_CAP_FRAME_THREADS,
                             AV_CODEC_CAP_SLICE_THREADS | AV_CODEC_CAP_FRAME_THREADS,
    .caps_internal         = FF_CODEC_CAP_INIT_THREADSAFE,
    .profiles              = NULL_IF_CONFIG_SMALL(ff_hevc_profiles),
};
