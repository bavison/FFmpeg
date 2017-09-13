#ifdef RPI
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "libavutil/avassert.h"

#include "config.h"

#include <pthread.h>
#include <time.h>

#include <interface/vcsm/user-vcsm.h>

#include "rpi_mailbox.h"
#include "rpi_qpu.h"
#include "rpi_shader.h"
#include "rpi_hevc_transform8.h"
#include "rpi_hevc_transform10.h"
#include "libavutil/rpi_sand_fns.h"

#pragma GCC diagnostic push
// Many many redundant decls in the header files
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include "interface/vmcs_host/vc_vchi_gpuserv.h"
#pragma GCC diagnostic pop

// Trace time spent waiting for GPU (VPU/QPU) (1=Yes, 0=No)
#define RPI_TRACE_TIME_VPU_QPU_WAIT     0

// Add profile flags to all QPU requests - generates output in "vcdbg log msg"
// Beware this is expensive and will probably throw off all other timing by >10%
#define RPI_TRACE_QPU_PROFILE_ALL       0

// QPU "noflush" flags
// a mixture of flushing & profiling

#define QPU_FLAGS_NO_FLUSH_VPU          1       // If unset VPU cache will be flushed
#define QPU_FLAGS_PROF_CLEAR_AND_ENABLE 2       // Clear & Enable detailed QPU profiling registers
#define QPU_FLAGS_PROF_OUTPUT_COUNTS    4       // Print the results
#define QPU_FLAGS_OUTPUT_QPU_TIMES      8       // Print QPU times - independant of the profiling
#define QPU_FLAGS_NO_FLUSH_QPU          16      // If unset flush QPU caches & TMUs (uniforms always flushed)

#define vcos_verify_ge0(x) ((x)>=0)

// Size in 32bit words
#define QPU_CODE_SIZE 4098
#define VPU_CODE_SIZE 2048

static const short rpi_transMatrix2even[32][16] = { // Even rows first
{64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64,  64},
{90,  87,  80,  70,  57,  43,  25,   9,  -9, -25, -43, -57, -70, -80, -87, -90},
{89,  75,  50,  18, -18, -50, -75, -89, -89, -75, -50, -18,  18,  50,  75,  89},
{87,  57,   9, -43, -80, -90, -70, -25,  25,  70,  90,  80,  43,  -9, -57, -87},
{83,  36, -36, -83, -83, -36,  36,  83,  83,  36, -36, -83, -83, -36,  36,  83},
{80,   9, -70, -87, -25,  57,  90,  43, -43, -90, -57,  25,  87,  70,  -9, -80},
{75, -18, -89, -50,  50,  89,  18, -75, -75,  18,  89,  50, -50, -89, -18,  75},
{70, -43, -87,   9,  90,  25, -80, -57,  57,  80, -25, -90,  -9,  87,  43, -70},
{64, -64, -64,  64,  64, -64, -64,  64,  64, -64, -64,  64,  64, -64, -64,  64},
{57, -80, -25,  90,  -9, -87,  43,  70, -70, -43,  87,   9, -90,  25,  80, -57},
{50, -89,  18,  75, -75, -18,  89, -50, -50,  89, -18, -75,  75,  18, -89,  50},
{43, -90,  57,  25, -87,  70,   9, -80,  80,  -9, -70,  87, -25, -57,  90, -43},
{36, -83,  83, -36, -36,  83, -83,  36,  36, -83,  83, -36, -36,  83, -83,  36},
{25, -70,  90, -80,  43,   9, -57,  87, -87,  57,  -9, -43,  80, -90,  70, -25},
{18, -50,  75, -89,  89, -75,  50, -18, -18,  50, -75,  89, -89,  75, -50,  18},
{ 9, -25,  43, -57,  70, -80,  87, -90,  90, -87,  80, -70,  57, -43,  25,  -9},
// Odd rows
{90,  90,  88,  85,  82,  78,  73,  67,  61,  54,  46,  38,  31,  22,  13,   4},
{90,  82,  67,  46,  22,  -4, -31, -54, -73, -85, -90, -88, -78, -61, -38, -13},
{88,  67,  31, -13, -54, -82, -90, -78, -46,  -4,  38,  73,  90,  85,  61,  22},
{85,  46, -13, -67, -90, -73, -22,  38,  82,  88,  54,  -4, -61, -90, -78, -31},
{82,  22, -54, -90, -61,  13,  78,  85,  31, -46, -90, -67,   4,  73,  88,  38},
{78,  -4, -82, -73,  13,  85,  67, -22, -88, -61,  31,  90,  54, -38, -90, -46},
{73, -31, -90, -22,  78,  67, -38, -90, -13,  82,  61, -46, -88,  -4,  85,  54},
{67, -54, -78,  38,  85, -22, -90,   4,  90,  13, -88, -31,  82,  46, -73, -61},
{61, -73, -46,  82,  31, -88, -13,  90,  -4, -90,  22,  85, -38, -78,  54,  67},
{54, -85,  -4,  88, -46, -61,  82,  13, -90,  38,  67, -78, -22,  90, -31, -73},
{46, -90,  38,  54, -90,  31,  61, -88,  22,  67, -85,  13,  73, -82,   4,  78},
{38, -88,  73,  -4, -67,  90, -46, -31,  85, -78,  13,  61, -90,  54,  22, -82},
{31, -78,  90, -61,   4,  54, -88,  82, -38, -22,  73, -90,  67, -13, -46,  85},
{22, -61,  85, -90,  73, -38,  -4,  46, -78,  90, -82,  54, -13, -31,  67, -88},
{13, -38,  61, -78,  88, -90,  85, -73,  54, -31,   4,  22, -46,  67, -82,  90},
{ 4, -13,  22, -31,  38, -46,  54, -61,  67, -73,  78, -82,  85, -88,  90, -90}
};

// Code/constants on GPU
struct GPU
{
  unsigned int qpu_code[QPU_CODE_SIZE];
  unsigned int vpu_code8[VPU_CODE_SIZE];
  unsigned int vpu_code10[VPU_CODE_SIZE];
  short transMatrix2even[16*16*2];
};

#define CFE_ENTS_PER_A 8
// If we have a sliced frame 2048 wide @ 64 per slice then there are 32 slices
// in a line & we want to flush luma + chroma + a couple of bits so ents ~ 70
// allow 128
#define CFE_ENT_COUNT  128
#define CFE_A_COUNT    (CFE_ENT_COUNT / CFE_ENTS_PER_A)

struct rpi_cache_flush_env_s {
    unsigned int n;
    struct vcsm_user_clean_invalid_s a[CFE_A_COUNT];
};

#define WAIT_COUNT_MAX 16

typedef struct trace_time_one_s
{
  int count;
  int64_t start[WAIT_COUNT_MAX];
  int64_t total[WAIT_COUNT_MAX];
} trace_time_one_t;

typedef struct trace_time_wait_s
{
  unsigned int jcount;
  int64_t start0;
  int64_t last_update;
  trace_time_one_t active;
  trace_time_one_t wait;
} trace_time_wait_t;

typedef struct vq_wait_s
{
  sem_t sem;
  struct vq_wait_s * next;
} vq_wait_t;

#define VQ_WAIT_POOL_SIZE 16
typedef struct vq_wait_pool_s
{
  vq_wait_t * head;
  vq_wait_t pool[VQ_WAIT_POOL_SIZE];
} vq_wait_pool_t;

static void vq_wait_pool_init(vq_wait_pool_t * const pool);
static void vq_wait_pool_deinit(vq_wait_pool_t * const pool);

typedef struct gpu_env_s
{
  int open_count;
  int init_count;
  int mb;
  int vpu_i_cache_flushed;
  GPU_MEM_PTR_T code_gm_ptr;
  vq_wait_pool_t wait_pool;
#if RPI_TRACE_TIME_VPU_QPU_WAIT
  trace_time_wait_t ttw;
#endif
} gpu_env_t;

// Stop more than one thread trying to allocate memory or use the processing resources at once
static pthread_mutex_t gpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static gpu_env_t * gpu = NULL;

#if RPI_TRACE_TIME_VPU_QPU_WAIT

static int64_t ns_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * (int64_t)1000000000 + ts.tv_nsec;
}


#define WAIT_TIME_PRINT_PERIOD (int64_t)2000000000

#define T_MS(t) ((unsigned int)((t)/(int64_t)1000000) % 1000U)
#define T_SEC(t) (unsigned int)((t)/(int64_t)1000000000)
#define T_ARG(t) T_SEC(t), T_MS(t)
#define T_FMT "%u.%03u"

static void tto_print(trace_time_one_t * tto, const int64_t now, const int64_t start0, const char * const prefix)
{
  // Update totals for levels that are still pending
  for (int i = 0; i < tto->count; ++i) {
    tto->total[i] += now - tto->start[i];
    tto->start[i] = now;
  }

  printf("%s: Idle:" T_FMT ", 1:" T_FMT ", 2:" T_FMT ", 3:" T_FMT ", 4:" T_FMT "\n",
         prefix,
         T_ARG(now - start0 - tto->total[0]),
         T_ARG(tto->total[0]),
         T_ARG(tto->total[1]),
         T_ARG(tto->total[2]),
         T_ARG(tto->total[3]));
}


static void tto_start(trace_time_one_t * const tto, const int64_t now)
{
  av_assert0(tto->count < WAIT_COUNT_MAX);
  tto->start[tto->count++] = now;
}

static void tto_end(trace_time_one_t * const tto, const int64_t now)
{
  const int n = --tto->count;
  av_assert0(n >= 0);
  tto->total[n] += now - tto->start[n];
}

static void ttw_print(trace_time_wait_t * const ttw, const int64_t now)
{
  printf("Jobs:%d, Total time=" T_FMT "\n", ttw->jcount, T_ARG(now - ttw->start0));
  tto_print(&ttw->active, now, ttw->start0, "Active");
  tto_print(&ttw->wait,   now, ttw->start0, "  Wait");
}

#endif

// GPU memory alloc fns (internal)

// GPU_MEM_PTR_T alloc fns
static int gpu_malloc_cached_internal(const int mb, const int numbytes, GPU_MEM_PTR_T * const p) {
  p->numbytes = numbytes;
  p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_HOST, (char *)"Video Frame" );
  //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_VC, (char *)"Video Frame" );
  //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_NONE, (char *)"Video Frame" );
  //p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_HOST_AND_VC, (char *)"Video Frame" );
  av_assert0(p->vcsm_handle);
  p->vc_handle = vcsm_vc_hdl_from_hdl(p->vcsm_handle);
  av_assert0(p->vc_handle);
  p->arm = vcsm_lock(p->vcsm_handle);
  av_assert0(p->arm);
  p->vc = mbox_mem_lock(mb, p->vc_handle);
  av_assert0(p->vc);
//  printf("***** %s, %d\n", __func__, numbytes);
  return 0;
}

static int gpu_malloc_uncached_internal(const int mb, const int numbytes, GPU_MEM_PTR_T * const p) {
  p->numbytes = numbytes;
  p->vcsm_handle = vcsm_malloc_cache(numbytes, VCSM_CACHE_TYPE_NONE, (char *)"Video Frame" );
  av_assert0(p->vcsm_handle);
  p->vc_handle = vcsm_vc_hdl_from_hdl(p->vcsm_handle);
  av_assert0(p->vc_handle);
  p->arm = vcsm_lock(p->vcsm_handle);
  av_assert0(p->arm);
  p->vc = mbox_mem_lock(mb, p->vc_handle);
  av_assert0(p->vc);
//  printf("***** %s, %d\n", __func__, numbytes);
  return 0;
}

static void gpu_free_internal(const int mb, GPU_MEM_PTR_T * const p) {
  mbox_mem_unlock(mb, p->vc_handle);
  vcsm_unlock_ptr(p->arm);
  vcsm_free(p->vcsm_handle);
  memset(p, 0, sizeof(*p));  // Ensure we crash hard if we try and use this again
//  printf("***** %s\n", __func__);
}


// GPU init, free, lock, unlock

static void gpu_term(void)
{
  gpu_env_t * const ge = gpu;

  // We have to hope that eveything has terminated...
  gpu = NULL;

  vc_gpuserv_deinit();

  gpu_free_internal(ge->mb, &ge->code_gm_ptr);

  vcsm_exit();

  mbox_close(ge->mb);

  vq_wait_pool_deinit(&ge->wait_pool);

  free(ge);
}


// Connect to QPU, returns 0 on success.
static int gpu_init(gpu_env_t ** const gpu) {
  volatile struct GPU* ptr;
  gpu_env_t * const ge = calloc(1, sizeof(gpu_env_t));
  *gpu = NULL;

  if (ge == NULL)
    return -1;

  if ((ge->mb = mbox_open()) < 0)
    return -1;

  vq_wait_pool_init(&ge->wait_pool);

  vcsm_init();

  gpu_malloc_uncached_internal(ge->mb, sizeof(struct GPU), &ge->code_gm_ptr);
  ptr = (volatile struct GPU*)ge->code_gm_ptr.arm;

  // Zero everything so we have zeros between the code bits
  memset((void *)ptr, 0, sizeof(*ptr));

  // Now copy over the QPU code into GPU memory
  {
    int num_bytes = (char *)mc_end - (char *)rpi_shader;
    av_assert0(num_bytes<=QPU_CODE_SIZE*sizeof(unsigned int));
    memcpy((void*)ptr->qpu_code, rpi_shader, num_bytes);
  }
  // And the VPU code
  {
    int num_bytes = sizeof(rpi_hevc_transform8);
    av_assert0(num_bytes<=VPU_CODE_SIZE*sizeof(unsigned int));
    memcpy((void*)ptr->vpu_code8, rpi_hevc_transform8, num_bytes);
  }
  {
    int num_bytes = sizeof(rpi_hevc_transform10);
    av_assert0(num_bytes<=VPU_CODE_SIZE*sizeof(unsigned int));
    memcpy((void*)ptr->vpu_code10, rpi_hevc_transform10, num_bytes);
  }
  // And the transform coefficients
  memcpy((void*)ptr->transMatrix2even, rpi_transMatrix2even, sizeof(rpi_transMatrix2even));

  *gpu = ge;
  return 0;
}



static void gpu_unlock(void) {
  pthread_mutex_unlock(&gpu_mutex);
}

// Make sure we have exclusive access to the mailbox, and enable qpu if necessary.
static gpu_env_t * gpu_lock(void) {
  pthread_mutex_lock(&gpu_mutex);

  av_assert0(gpu != NULL);
  return gpu;
}

static gpu_env_t * gpu_lock_ref(void)
{
  pthread_mutex_lock(&gpu_mutex);

  if (gpu == NULL) {
    int rv = gpu_init(&gpu);
    if (rv != 0) {
      gpu_unlock();
      return NULL;
    }
  }

  ++gpu->open_count;
  return gpu;
}

static void gpu_unlock_unref(gpu_env_t * const ge)
{
  if (--ge->open_count == 0)
    gpu_term();

  gpu_unlock();
}

static inline gpu_env_t * gpu_ptr(void)
{
  av_assert0(gpu != NULL);
  return gpu;
}

// Public gpu fns

// Allocate memory on GPU
// Fills in structure <p> containing ARM pointer, videocore handle, videocore memory address, numbytes
// Returns 0 on success.
// This allocates memory that will not be cached in ARM's data cache.
// Therefore safe to use without data cache flushing.
int gpu_malloc_uncached(int numbytes, GPU_MEM_PTR_T *p)
{
  int r;
  gpu_env_t * const ge = gpu_lock_ref();
  if (ge == NULL)
    return -1;
  r = gpu_malloc_uncached_internal(ge->mb, numbytes, p);
  gpu_unlock();
  return r;
}

// This allocates data that will be
//    Cached in ARM L2
//    Uncached in VPU L2
int gpu_malloc_cached(int numbytes, GPU_MEM_PTR_T *p)
{
  int r;
  gpu_env_t * const ge = gpu_lock_ref();
  if (ge == NULL)
    return -1;
  r = gpu_malloc_cached_internal(ge->mb, numbytes, p);
  gpu_unlock();
  return r;
}

void gpu_free(GPU_MEM_PTR_T * const p) {
  gpu_env_t * const ge = gpu_lock();
  gpu_free_internal(ge->mb, p);
  gpu_unlock_unref(ge);
}

unsigned int vpu_get_fn(const unsigned int bit_depth) {
  // Make sure that the gpu is initialized
  av_assert0(gpu != NULL);
  switch (bit_depth){
    case 8:
      return gpu->code_gm_ptr.vc + offsetof(struct GPU, vpu_code8);
    case 10:
      return gpu->code_gm_ptr.vc + offsetof(struct GPU, vpu_code10);
    default:
      av_assert0(0);
  }
  return 0;
}

unsigned int vpu_get_constants(void) {
  av_assert0(gpu != NULL);
  return gpu->code_gm_ptr.vc + offsetof(struct GPU,transMatrix2even);
}

int gpu_get_mailbox(void)
{
  av_assert0(gpu);
  return gpu->mb;
}

void gpu_ref(void)
{
  gpu_lock_ref();
  gpu_unlock();
}

void gpu_unref(void)
{
  gpu_env_t * const ge = gpu_lock();
  gpu_unlock_unref(ge);
}

// ----------------------------------------------------------------------------
//
// Cache flush functions


rpi_cache_flush_env_t * rpi_cache_flush_init()
{
    rpi_cache_flush_env_t * const rfe = malloc(sizeof(rpi_cache_flush_env_t));
    if (rfe == NULL)
        return NULL;

    rfe->n = 0;
    return rfe;
}

void rpi_cache_flush_abort(rpi_cache_flush_env_t * const rfe)
{
    if (rfe != NULL)
        free(rfe);
}

int rpi_cache_flush_finish(rpi_cache_flush_env_t * const rfe)
{
    int rc = 0;
    unsigned int na;
    unsigned int nr;

    // Clear any reamaining ents in the final block
    if ((nr = rfe->n % CFE_ENTS_PER_A) != 0)
        memset(rfe->a[rfe->n / CFE_ENTS_PER_A].s + nr, 0, (CFE_ENTS_PER_A - nr) * sizeof(rfe->a[0].s[0]));

    for (na = 0; na * CFE_ENTS_PER_A < rfe->n; ++na)
    {
        if (vcsm_clean_invalid(rfe->a + na) != 0)
            rc = -1;
    }

    free(rfe);

    if (rc == 0)
        return 0;

    av_log(NULL, AV_LOG_ERROR, "vcsm_clean_invalid failed: errno=%d\n", errno);
    return rc;
}

void rpi_cache_flush_add_gm_ptr(rpi_cache_flush_env_t * const rfe, const GPU_MEM_PTR_T * const gm, const unsigned int mode)
{
    // Deal with empty pointer trivially
    if (gm == NULL || gm->numbytes == 0)
        return;

    {
        struct vcsm_user_clean_invalid_s * const a = rfe->a + (rfe->n / CFE_ENTS_PER_A);
        const unsigned int n = rfe->n % CFE_ENTS_PER_A;

        av_assert0(rfe->n < CFE_ENT_COUNT);

        a->s[n].cmd = mode;
        a->s[n].handle = gm->vcsm_handle;
        a->s[n].addr = (unsigned int)gm->arm;
        a->s[n].size = gm->numbytes;
        ++rfe->n;
    }
}

void rpi_cache_flush_add_gm_range(rpi_cache_flush_env_t * const rfe, const GPU_MEM_PTR_T * const gm, const unsigned int mode,
  const unsigned int offset, const unsigned int size)
{
    // Deal with empty pointer trivially
    if (gm == NULL || size == 0)
        return;

//    printf("[%d] offset=%d, size=%d, numbytes=%d\n", rfe->n, offset, size, gm->numbytes);

    av_assert0(offset <= gm->numbytes);
    av_assert0(size <= gm->numbytes);
    av_assert0(offset + size <= gm->numbytes);

    {
        struct vcsm_user_clean_invalid_s * const a = rfe->a + (rfe->n / CFE_ENTS_PER_A);
        const unsigned int n = rfe->n % CFE_ENTS_PER_A;

        av_assert0(rfe->n < CFE_ENT_COUNT);

        a->s[n].cmd = mode;
        a->s[n].handle = gm->vcsm_handle;
        a->s[n].addr = (unsigned int)gm->arm + offset;
        a->s[n].size = size;
        ++rfe->n;
    }
}

void rpi_cache_flush_add_frame(rpi_cache_flush_env_t * const rfe, const AVFrame * const frame, const unsigned int mode)
{
#if !RPI_ONE_BUF
#error Fixme! (NIF)
#endif
  if (gpu_is_buf1(frame)) {
    rpi_cache_flush_add_gm_ptr(rfe, gpu_buf1_gmem(frame), mode);
  }
  else
  {
    rpi_cache_flush_add_gm_ptr(rfe, gpu_buf3_gmem(frame, 0), mode);
    rpi_cache_flush_add_gm_ptr(rfe, gpu_buf3_gmem(frame, 1), mode);
    rpi_cache_flush_add_gm_ptr(rfe, gpu_buf3_gmem(frame, 2), mode);
  }
}

// Flush an area of a frame
// Width, height, x0, y0 in luma pels
void rpi_cache_flush_add_frame_block(rpi_cache_flush_env_t * const rfe, const AVFrame * const frame, const unsigned int mode,
  const unsigned int x0, const unsigned int y0, const unsigned int width, const unsigned int height,
  const unsigned int uv_shift, const int do_luma, const int do_chroma)
{
  const unsigned int y_offset = frame->linesize[0] * y0;
  const unsigned int y_size = frame->linesize[0] * height;
  // Round UV up/down to get everything
  const unsigned int uv_rnd = (1U << uv_shift) >> 1;
  const unsigned int uv_offset = frame->linesize[1] * (y0 >> uv_shift);
  const unsigned int uv_size = frame->linesize[1] * ((y0 + height + uv_rnd) >> uv_shift) - uv_offset;

#if 0
  // *** frame->height is cropped height so not good
  // As all unsigned they will also reject -ve
  // Test individually as well as added to reject overflow
  av_assert0(start_line <= (unsigned int)frame->height);  // ***** frame height cropped
  av_assert0(n <= (unsigned int)frame->height);
  av_assert0(start_line + n <= (unsigned int)frame->height);
#endif

  if (!gpu_is_buf1(frame))
  {
    if (do_luma) {
      rpi_cache_flush_add_gm_range(rfe, gpu_buf3_gmem(frame, 0), mode, y_offset, y_size);
    }
    if (do_chroma) {
      rpi_cache_flush_add_gm_range(rfe, gpu_buf3_gmem(frame, 1), mode, uv_offset, uv_size);
      rpi_cache_flush_add_gm_range(rfe, gpu_buf3_gmem(frame, 2), mode, uv_offset, uv_size);
    }
  }
  else if (!av_rpi_is_sand_frame(frame))
  {
    const GPU_MEM_PTR_T * const gm = gpu_buf1_gmem(frame);
    if (do_luma) {
      rpi_cache_flush_add_gm_range(rfe, gm, mode, (frame->data[0] - gm->arm) + y_offset, y_size);
    }
    if (do_chroma) {
      rpi_cache_flush_add_gm_range(rfe, gm, mode, (frame->data[1] - gm->arm) + uv_offset, uv_size);
      rpi_cache_flush_add_gm_range(rfe, gm, mode, (frame->data[2] - gm->arm) + uv_offset, uv_size);
    }
  }
  else
  {
    const GPU_MEM_PTR_T * const gm = gpu_buf1_gmem(frame);
//    printf("%s: start_line=%d, lines=%d, %c%c\n", __func__, start_line, n, do_luma ? 'l' : ' ', do_chroma ? 'c' : ' ');
    // **** Use x0!
    // We are working in pels here so halve linesize if 16-bit frame
    const unsigned int slice_width = av_rpi_is_sand8_frame(frame) ? frame->linesize[0] : (frame->linesize[0] >> 1);
    for (unsigned int x = 0; x < x0 + width; x += slice_width) {
      if (do_luma) {
        rpi_cache_flush_add_gm_range(rfe, gm, mode, av_rpi_sand_frame_off_y(frame, x, y0), y_size);
      }
      if (do_chroma) {
        rpi_cache_flush_add_gm_range(rfe, gm, mode,
                                     (frame->data[1] - gm->arm) + av_rpi_sand_frame_off_c(frame, x >> 1, y0 >> 1), uv_size);
      }
    }
  }
}

// Call this to clean and invalidate a region of memory
void rpi_cache_flush_one_gm_ptr(const GPU_MEM_PTR_T *const p, const rpi_cache_flush_mode_t mode)
{
  rpi_cache_flush_env_t * rfe = rpi_cache_flush_init();
  rpi_cache_flush_add_gm_ptr(rfe, p, mode);
  rpi_cache_flush_finish(rfe);
}


// ----------------------------------------------------------------------------


// Wait abstractions - mostly so we can easily add profile code
static void vq_wait_pool_init(vq_wait_pool_t * const wp)
{
  unsigned int i;
  for (i = 0; i != VQ_WAIT_POOL_SIZE; ++i) {
    sem_init(&wp->pool[i].sem, 0, 0);
    wp->pool[i].next = wp->pool + i + 1;
  }
  wp->head = wp->pool + 0;
  wp->pool[VQ_WAIT_POOL_SIZE - 1].next = NULL;
}

static void vq_wait_pool_deinit(vq_wait_pool_t * const wp)
{
  unsigned int i;
  wp->head = NULL;
  for (i = 0; i != VQ_WAIT_POOL_SIZE; ++i) {
    sem_destroy(&wp->pool[i].sem);
    wp->pool[i].next = NULL;
  }
}


// If sem_init actually takes time then maybe we want a pool...
static vq_wait_t * vq_wait_new(void)
{
  gpu_env_t * const ge = gpu_lock_ref();
  vq_wait_t * const wait = ge->wait_pool.head;
  ge->wait_pool.head = wait->next;
  wait->next = NULL;

#if RPI_TRACE_TIME_VPU_QPU_WAIT
  tto_start(&ge->ttw.active, ns_time());
#endif

  gpu_unlock();
  return wait;
}

static void vq_wait_delete(vq_wait_t * const wait)
{
  gpu_env_t * const ge = gpu_lock();
  wait->next = ge->wait_pool.head;
  ge->wait_pool.head = wait;

#if RPI_TRACE_TIME_VPU_QPU_WAIT
  {
    trace_time_wait_t * const ttw = &ge->ttw;
    const int64_t now = ns_time();
    ++ttw->jcount;
    tto_end(&ttw->wait, now);

    if (ttw->start0 == 0)
    {
      ttw->start0 = ttw->active.start[0];
      ttw->last_update = ttw->start0;
    }
    if (now - ttw->last_update > WAIT_TIME_PRINT_PERIOD)
    {
      ttw->last_update += WAIT_TIME_PRINT_PERIOD;
      ttw_print(ttw, now);
    }
  }
#endif
  gpu_unlock_unref(ge);
}

static void vq_wait_wait(vq_wait_t * const wait)
{
#if RPI_TRACE_TIME_VPU_QPU_WAIT
  {
      const int64_t now = ns_time();
      gpu_env_t * const ge = gpu_lock();
      tto_start(&ge->ttw.wait, now);
      gpu_unlock();
  }
#endif

  while (sem_wait(&wait->sem) == -1 && errno == EINTR)
    /* loop */;
}

static void vq_wait_post(vq_wait_t * const wait)
{
#if RPI_TRACE_TIME_VPU_QPU_WAIT
  {
    gpu_env_t *const ge = gpu_lock();
    tto_end(&ge->ttw.active, ns_time());
    gpu_unlock();
  }
#endif

  sem_post(&wait->sem);
}



// Header comments were wrong for these two
#define VPU_QPU_MASK_QPU  1
#define VPU_QPU_MASK_VPU  2

#define VPU_QPU_JOB_MAX 4
struct vpu_qpu_job_env_s
{
  unsigned int n;
  unsigned int mask;
  struct gpu_job_s j[VPU_QPU_JOB_MAX];
};

typedef struct vpu_qpu_job_env_s vpu_qpu_job_env_t;

vpu_qpu_job_env_t * vpu_qpu_job_new(void)
{
  vpu_qpu_job_env_t * vqj = calloc(1, sizeof(vpu_qpu_job_env_t));
  return vqj;
}

void vpu_qpu_job_delete(vpu_qpu_job_env_t * const vqj)
{
  memset(vqj, 0, sizeof(*vqj));
  free(vqj);
}

static inline struct gpu_job_s * new_job(vpu_qpu_job_env_t * const vqj)
{
  struct gpu_job_s * const j = vqj->j + vqj->n++;
  av_assert0(vqj->n <= VPU_QPU_JOB_MAX);
  return j;
}

void vpu_qpu_job_add_vpu(vpu_qpu_job_env_t * const vqj, const uint32_t vpu_code,
  const unsigned r0, const unsigned r1, const unsigned r2, const unsigned r3, const unsigned r4, const unsigned r5)
{
  if (vpu_code != 0) {
    struct gpu_job_s *const j = new_job(vqj);
    vqj->mask |= VPU_QPU_MASK_VPU;

    j->command = EXECUTE_VPU;
    // The bottom two bits of the execute address contain no-flush flags
    // b0 will flush the VPU I-cache if unset so we nearly always want that set
    // as we never reload code
    j->u.v.q[0] = vpu_code | gpu->vpu_i_cache_flushed;
    j->u.v.q[1] = r0;
    j->u.v.q[2] = r1;
    j->u.v.q[3] = r2;
    j->u.v.q[4] = r3;
    j->u.v.q[5] = r4;
    j->u.v.q[6] = r5;
    gpu->vpu_i_cache_flushed = 1;
  }
}

// flags are QPU_FLAGS_xxx
void vpu_qpu_job_add_qpu(vpu_qpu_job_env_t * const vqj, const unsigned int n, const uint32_t * const mail)
{
  if (n != 0) {
    struct gpu_job_s *const j = new_job(vqj);
    vqj->mask |= VPU_QPU_MASK_QPU;

    j->command = EXECUTE_QPU;
    j->u.q.jobs = n;
#if RPI_TRACE_QPU_PROFILE_ALL
    j->u.q.noflush = QPU_FLAGS_NO_FLUSH_VPU | QPU_FLAGS_PROF_CLEAR_AND_ENABLE | QPU_FLAGS_PROF_OUTPUT_COUNTS;
#else
    j->u.q.noflush = QPU_FLAGS_NO_FLUSH_VPU;
#endif
    j->u.q.timeout = 5000;
    memcpy(j->u.q.control, mail, n * QPU_MAIL_EL_VALS * sizeof(uint32_t));
  }
}

// Convert callback to sem post
static void vpu_qpu_job_callback_wait(void * v)
{
  vq_wait_post(v);
}

void vpu_qpu_job_add_sync_this(vpu_qpu_job_env_t * const vqj, vpu_qpu_wait_h * const wait_h)
{
  vq_wait_t * wait;

  if (vqj->mask == 0) {
    *wait_h = NULL;
    return;
  }

  // We are going to want a sync object
  wait = vq_wait_new();

  // There are 2 VPU Qs & 1 QPU Q so we can collapse sync
  // If we only posted one thing or only QPU jobs
  if (vqj->n == 1 || vqj->mask == VPU_QPU_MASK_QPU)
  {
    struct gpu_job_s * const j = vqj->j + (vqj->n - 1);
    av_assert0(j->callback.func == 0);

    j->callback.func = vpu_qpu_job_callback_wait;
    j->callback.cookie = wait;
  }
  else
  {
    struct gpu_job_s *const j = new_job(vqj);

    j->command = EXECUTE_SYNC;
    j->u.s.mask = vqj->mask;
    j->callback.func = vpu_qpu_job_callback_wait;
    j->callback.cookie = wait;
  }

  vqj->mask = 0;
  *wait_h = wait;
}

int vpu_qpu_job_start(vpu_qpu_job_env_t * const vqj)
{
  return vqj->n == 0 ? 0 : vc_gpuserv_execute_code(vqj->n, vqj->j);
}

// Simple wrapper of start + delete
int vpu_qpu_job_finish(vpu_qpu_job_env_t * const vqj)
{
  int rv;
  rv = vpu_qpu_job_start(vqj);
  vpu_qpu_job_delete(vqj);
  return rv;
}

void vpu_qpu_wait(vpu_qpu_wait_h * const wait_h)
{
  if (wait_h != NULL)
  {
    vq_wait_t * const wait = *wait_h;
    if (wait != NULL) {
      *wait_h = NULL;
      vq_wait_wait(wait);
      vq_wait_delete(wait);
    }
  }
}

#include <dirent.h>
#include <unistd.h>

typedef struct thread_list
{
    struct thread_list *next;
    pid_t tid;
} thread_list_t;

/* We're not expecting these lists to be particularly long, so don't worry about making
 * these routines super-efficient! */

static void add_to_list(pid_t tid, void *arg)
{
    thread_list_t **list = arg;
    thread_list_t *t = malloc(sizeof *t);
    if (t)
    {
        t->tid = tid;
        t->next = *list;
        *list = t;
    }
}

static void list_subtract(thread_list_t **base, const thread_list_t *delta)
{
    thread_list_t *p, *t;
    for (; delta; delta = delta->next)
    {
        for (p = NULL, t = *base; t && t->tid != delta->tid; p = t, t = t->next);
        if (p)
            p->next = t->next;
        else
            *base = t->next;
        free(t);
    }
}

static void list_free(thread_list_t *list)
{
    thread_list_t *next;
    for (; list; list = next)
    {
        next = list->next;
        free(list);
    }
}

static void for_all_threads(void (*callback)(pid_t, void *), void *arg)
{
    DIR *d;
    pid_t pid = getpid();
    char path[20];
    sprintf(path, "/proc/%u/task", pid);
    if ((d = opendir(path)) != NULL)
    {
        struct dirent *de;
        while ((de = readdir(d)) != NULL)
        {
            if (de->d_name[0] >= '0' && de->d_name[0] <= '9')
            {
                pid_t tid = atoi(de->d_name);
                callback(tid, arg);
            }
        }
    }
}

int vpu_qpu_init()
{
  gpu_env_t * const ge = gpu_lock_ref();
  if (ge == NULL)
    return -1;

  if (ge->init_count++ == 0)
  {
    thread_list_t *before = NULL, *after = NULL;

    for_all_threads(add_to_list, &before);
    vc_gpuserv_init();
    for_all_threads(add_to_list, &after);
    list_subtract(&after, before);
    for (thread_list_t *t = after; t; t = t->next)
    {
        struct sched_param sched_param = { .sched_priority = 95 };
        int result = sched_setscheduler(t->tid, SCHED_FIFO, &sched_param);
        if (result != 0)
            perror("vpu_qpu_init");
    }
    list_free(before);
    list_free(after);
  }

  gpu_unlock();
  return 0;
}

void vpu_qpu_term()
{
  gpu_env_t * const ge = gpu_lock();

  if (--ge->init_count == 0) {
    vc_gpuserv_deinit();

#if RPI_TRACE_TIME_VPU_QPU_WAIT
    ttw_print(&ge->ttw, ns_time());
#endif
  }

  gpu_unlock_unref(ge);
}

uint32_t qpu_fn(const int * const mc_fn)
{
  return gpu->code_gm_ptr.vc + ((const char *)mc_fn - (const char *)rpi_shader) + offsetof(struct GPU, qpu_code);
}


int rpi_hevc_qpu_init_fn(HEVCRpiQpu * const qf, const unsigned int bit_depth)
{
  // Dummy values we can catch with emulation
  qf->y_pxx = ~1U;
  qf->y_bxx = ~2U;
  qf->y_p00 = ~3U;
  qf->y_b00 = ~4U;
  qf->c_pxx = ~5U;
  qf->c_bxx = ~6U;

  switch (bit_depth) {
    case 8:
      qf->y_pxx = qpu_fn(mc_filter_y_pxx);
      qf->y_pxx = qpu_fn(mc_filter_y_pxx);
      qf->y_bxx = qpu_fn(mc_filter_y_bxx);
      qf->y_p00 = qpu_fn(mc_filter_y_p00);
      qf->y_b00 = qpu_fn(mc_filter_y_b00);
      qf->c_pxx = qpu_fn(mc_filter_c_p);
      qf->c_pxx_l1 = qpu_fn(mc_filter_c_p_l1);
      qf->c_bxx = qpu_fn(mc_filter_c_b);
      break;
    case 10:
      qf->c_pxx = qpu_fn(mc_filter_c10_p);
      qf->c_pxx_l1 = qpu_fn(mc_filter_c10_p_l1);
      qf->c_bxx = qpu_fn(mc_filter_c10_b);
      qf->y_pxx = qpu_fn(mc_filter_y10_pxx);
      qf->y_bxx = qpu_fn(mc_filter_y10_bxx);
      qf->y_p00 = qpu_fn(mc_filter_y10_p00);
      qf->y_b00 = qpu_fn(mc_filter_y10_b00);
      break;
    default:
      return -1;
  }
  return 0;
}

#endif // RPI
