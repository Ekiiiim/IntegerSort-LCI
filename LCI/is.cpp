/*************************************************************************
 *                                                                       *
 *        N  A  S     P A R A L L E L     B E N C H M A R K S  3.4       *
 *                                                                       *
 *                                  I S                                  *
 *                                                                       *
 *************************************************************************
 *                                                                       *
 *   This benchmark is part of the NAS Parallel Benchmark 3.4 suite.     *
 *   It is described in NAS Technical Report 95-020.                     *
 *                                                                       *
 *   Permission to use, copy, distribute and modify this software        *
 *   for any purpose with or without fee is hereby granted.  We          *
 *   request, however, that all derived work reference the NAS           *
 *   Parallel Benchmarks 3.4. This software is provided "as is"          *
 *   without express or implied warranty.                                *
 *                                                                       *
 *   Information on NPB 3.4, including the technical report, the         *
 *   original specifications, source code, results and information       *
 *   on how to submit new results, is available at:                      *
 *                                                                       *
 *          http://www.nas.nasa.gov/Software/NPB                         *
 *                                                                       *
 *   Send comments or suggestions to  npb@nas.nasa.gov                   *
 *                                                                       *
 *         NAS Parallel Benchmarks Group                                 *
 *         NASA Ames Research Center                                     *
 *         Moffett Field, CA   94035-1000                                *
 *                                                                       *
 *************************************************************************
 *                                                                       *
 *   Author: M. Yarrow                                                   *
 *           H. Jin                                                      *
 *                                                                       *
 *************************************************************************/

/*************************************************************************
 *  LCI + OpenMP variant of NPB IS -- Multithreaded Fine-grained
 *  Asynchronous BSP (FA-BSP).
 *
 *  Each rank is one process with multiple OpenMP threads, communicating
 *  through LCI. The workflow is:
 *
 *    Step 1  is_lci::generate_keys()  Generate Gaussian-distributed keys.
 *    Step 2  rank()        Bin keys into buckets; reduce to global bucket
 *                          sizes.
 *    Step 3  rank()        Greedily assign buckets to processes
 *                          (coarse load balancing).
 *    Step 4  rank()        Redistribute keys with fine-grained active
 *                          messages instead of MPI_Alltoallv: each thread
 *                          batches keys into per-destination send buffers,
 *                          flushes them when full, and concurrently
 *                          progresses LCI to receive and tally keys.
 *    Step 5  rank()        Parallel prefix sum over the key-frequency
 *                          array to obtain the final key ranks.
 *
 *  Step 1 runs once; Steps 2-5 run every timed iteration in rank().
 *************************************************************************/

#include "a2a_tl_timers.hpp"
#include "bucket_plan.hpp"
#include "is_config.hpp"
#include "key_generation.hpp"
#include "lci_redistributor.hpp"
#include "ranking.hpp"

#include <lci.hpp>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <omp.h>
#include <atomic>
#include <vector>

using INT_TYPE = is_lci::Count;
using KEY_TYPE = is_lci::Rank;

/*****************************************************************/
/* Number of keys batched into one active message (Step 4).      */
/* Capped to the eager-protocol limit; set in alloc_space().     */
/*****************************************************************/
int msg_batch_size;
int NUM_THREADS_PER_PROC;
int NUM_DEVICES;
int NUM_THREADS_PER_DEVICE;

/* Number of keys assigned to each processor
 * #define  NUM_KEYS            (TOTAL_KEYS/NUM_PROCS*MIN_PROCS)
 */
int num_keys;

/*****************************************************************/
/* On larger number of processors, since the keys are (roughly)  */
/* gaussian distributed, the first and last processor sort keys  */
/* in a large interval, requiring array sizes to be larger. Note */
/* that for large NUM_PROCS, NUM_KEYS is, however, a small number*/
/* The required array size also depends on the bucket size used. */
/* The following values are validated for the 1024-bucket setup. */
/*****************************************************************/
/*
 * #if   NUM_PROCS < 256
 * #define  SIZE_OF_BUFFERS     3*NUM_KEYS/2
 * #elif NUM_PROCS < 512
 * #define  SIZE_OF_BUFFERS     5*NUM_KEYS/2
 * #elif NUM_PROCS < 1024
 * #define  SIZE_OF_BUFFERS     4*NUM_KEYS
 * #else
 * #define  SIZE_OF_BUFFERS     13*NUM_KEYS/2
 * #endif
 */
int size_of_buffers;

/***********************************/
/* Enable separate communication,  */
/* computation timing and printout */
/***********************************/
#define  TIMING_ENABLED
#ifdef NO_MTIMERS
#undef TIMINIG_ENABLED
#define TIMER_START( x )
#define TIMER_STOP( x )
#else
#define TIMER_START( x ) if (timeron) timer_start( x )
#define TIMER_STOP( x ) if (timeron) timer_stop( x )
#define T_TOTAL  0
#define T_RANK   1
#define T_RCOMM  2
#define T_VERIFY 3
#define T_ALLTOALL 4
#define T_RANK_1 5
#define T_RANK_2 6
#define T_RANK_3 7
#define T_RANK_1_1 8
#define T_LAST   9
#endif
int timeron;
int use_upacket;
int use_loopback;

#define MP_KEY_TYPE MPI_INT

/********************/
/* MPI properties:  */
/********************/
int      my_rank, np_total,
         comm_size;

/********************/
/* LCI properties:  */
/********************/
is_lci::RedistributorRuntime redistributor_runtime;
std::vector<lci::device_t> devices;

/********************/
/* Some global info */
/********************/
std::atomic<INT_TYPE> *key_buff_ptr_global,         /* used by full_verify to get */
                      *key_buff_ptr,
                      *key_buff1;
KEY_TYPE *cumulative_key_buff_ptr;
INT_TYPE total_local_keys;             /* copies of rank info        */
INT_TYPE min_key_val_global, max_key_val_global;

int      passed_verification;

/************************************/
/* These are the three main arrays. */
/* See SIZE_OF_BUFFERS def above    */
/************************************/
INT_TYPE *key_array,
         bucket_size[NUM_BUCKETS+TEST_ARRAY_SIZE],     /* Top 5 elements for */
         bucket_size_totals[NUM_BUCKETS+TEST_ARRAY_SIZE], /* part. ver. vals */
         process_bucket_distrib_ptr1[NUM_BUCKETS+TEST_ARRAY_SIZE],
         process_bucket_distrib_ptr2[NUM_BUCKETS+TEST_ARRAY_SIZE],
         bucket_i_to_process_ranks[NUM_BUCKETS+TEST_ARRAY_SIZE];

/**********************/
/* Partial verif info */
/**********************/
KEY_TYPE test_index_array[TEST_ARRAY_SIZE],
         test_rank_array[TEST_ARRAY_SIZE];

int      S_test_index_array[TEST_ARRAY_SIZE] =
                             {48427,17148,23627,62548,4431},
         S_test_rank_array[TEST_ARRAY_SIZE] =
                             {0,18,346,64917,65463},

         W_test_index_array[TEST_ARRAY_SIZE] =
                             {357773,934767,875723,898999,404505},
         W_test_rank_array[TEST_ARRAY_SIZE] =
                             {1249,11698,1039987,1043896,1048018},

         A_test_index_array[TEST_ARRAY_SIZE] =
                             {2112377,662041,5336171,3642833,4250760},
         A_test_rank_array[TEST_ARRAY_SIZE] =
                             {104,17523,123928,8288932,8388264},

         B_test_index_array[TEST_ARRAY_SIZE] =
                             {41869,812306,5102857,18232239,26860214},
         B_test_rank_array[TEST_ARRAY_SIZE] =
                             {33422937,10244,59149,33135281,99},

         C_test_index_array[TEST_ARRAY_SIZE] =
                             {44172927,72999161,74326391,129606274,21736814},
         C_test_rank_array[TEST_ARRAY_SIZE] =
                             {61147,882988,266290,133997595,133525895};

long     D_test_index_array[TEST_ARRAY_SIZE] =
                             {1317351170,995930646,1157283250,1503301535,1453734525},
         D_test_rank_array[TEST_ARRAY_SIZE] =
                             {1,36538729,1978098519,2145192618,2147425337},

         E_test_index_array[TEST_ARRAY_SIZE] =
                             {21492309536L,24606226181L,12608530949L,4065943607L,3324513396L},
         E_test_rank_array[TEST_ARRAY_SIZE] =
                             {3L,27580354L,3248475153L,30048754302L,31485259697L};

void full_verify( void );

#ifdef __cplusplus
extern "C" {
#endif

void c_print_results( const char   *name,
                      char   _class,
                      int    n1,
                      int    n2,
                      int    n3,
                      int    niter,
                      int    nprocs_active,
                      int    nprocs_total,
                      double t,
                      double mops,
		      const char   *optype,
                      int    passed_verification,
                      const char   *npbversion,
                      const char   *compiletime,
                      const char   *mpicc,
                      const char   *clink,
                      const char   *cmpi_lib,
                      const char   *cmpi_inc,
                      const char   *cflags,
                      const char   *clinkflags );

#ifdef __cplusplus
}
#endif

#include "../common/c_timers.h"
/*****************************************************************/
/*     Dynamically allocate space for main arrays                */
/*****************************************************************/
void alloc_space(void)
{
   /* problem size after partition */
   num_keys = (TOTAL_KEYS/comm_size) * MIN_PROCS;
   /* Set batch size to be less than or equal to get_max_bcopy_size() */
   /* to ensure use of eager protocol. This value can be set using */
   /* env variable LCI_ATTR_PACKET_SIZE. */
   msg_batch_size = lci::get_max_bcopy_size() / sizeof(INT_TYPE) - 1;

   /* buffer size for communication */
   if ( comm_size < 256 )
      size_of_buffers = 3*num_keys/2;
   else if ( comm_size < 512 )
      size_of_buffers = 5*num_keys/2;
   else if ( comm_size < 1024 )
      size_of_buffers = 4*num_keys;
   else
      size_of_buffers = 13*num_keys/2;

   /* allocate space */
   key_array = (INT_TYPE *)malloc(sizeof(INT_TYPE)*size_of_buffers);
   key_buff1 = (std::atomic<INT_TYPE> *)malloc(sizeof(std::atomic<INT_TYPE>)*size_of_buffers);
   cumulative_key_buff_ptr = (KEY_TYPE *)malloc(sizeof(KEY_TYPE)*size_of_buffers);

   if (!key_array || !key_buff1 || !cumulative_key_buff_ptr) {
      printf("ERROR: memory allocation failed\n");
      lci::g_runtime_fina();
      exit(1);
   }
}

/*****************************************************************/
/*     Free dynamically allocated space                          */
/*****************************************************************/
void free_space(void)
{
   free(key_array);
   free(key_buff1);
   free(cumulative_key_buff_ptr);

   key_array = NULL;
   key_buff1 = NULL;
   cumulative_key_buff_ptr = NULL;
}

/*****************************************************************/
/*************   C  H  E  C  K  _  U  S  E  _  U  P  A  C  K  E  T  **/
/*****************************************************************/
int check_use_upacket_flag( void )
{
    int upacket_on = 1;  // default to enabled
    char *ev = getenv("USE_UPACKET");

    if (ev) {
        if (*ev == '\0')
            upacket_on = 1;
        else if (*ev >= '1' && *ev <= '9')
            upacket_on = 1;
        else if (strcmp(ev, "on") == 0 || strcmp(ev, "ON") == 0 ||
                 strcmp(ev, "yes") == 0 || strcmp(ev, "YES") == 0 ||
                 strcmp(ev, "true") == 0 || strcmp(ev, "TRUE") == 0)
            upacket_on = 1;
        else if (strcmp(ev, "off") == 0 || strcmp(ev, "OFF") == 0 ||
                 strcmp(ev, "no") == 0 || strcmp(ev, "NO") == 0 ||
                 strcmp(ev, "false") == 0 || strcmp(ev, "FALSE") == 0 ||
                 strcmp(ev, "0") == 0)
            upacket_on = 0;
    }

    return upacket_on;
}

/*****************************************************************/
/*************   C  H  E  C  K  _  L  O  O  P  B  A  C  K  **********/
/*****************************************************************/
int check_loopback_flag( void )
{
    int loopback_on = 1;  // default to enabled
    char *ev = getenv("LOOPBACK");

    if (ev) {
        if (*ev == '\0')
            loopback_on = 1;
        else if (*ev >= '1' && *ev <= '9')
            loopback_on = 1;
        else if (strcmp(ev, "on") == 0 || strcmp(ev, "ON") == 0 ||
                 strcmp(ev, "yes") == 0 || strcmp(ev, "YES") == 0 ||
                 strcmp(ev, "true") == 0 || strcmp(ev, "TRUE") == 0)
            loopback_on = 1;
        else if (strcmp(ev, "off") == 0 || strcmp(ev, "OFF") == 0 ||
                 strcmp(ev, "no") == 0 || strcmp(ev, "NO") == 0 ||
                 strcmp(ev, "false") == 0 || strcmp(ev, "FALSE") == 0 ||
                 strcmp(ev, "0") == 0)
            loopback_on = 0;
    }

    return loopback_on;
}

/*****************************************************************/
/*************    F  U  L  L  _  V  E  R  I  F  Y     ************/
/*****************************************************************/

void full_verify( void )
{
    lci::comp_t sync = lci::alloc_sync();
    lci::comp_t sync_send = lci::alloc_sync();

    INT_TYPE    i, j;
    INT_TYPE    k, last_local_key;

    TIMER_START( T_VERIFY );

/*  Now, finally, sort the keys:  */
    INT_TYPE idx = 0;
    for (KEY_TYPE k = min_key_val_global; k <= max_key_val_global; ++k) {
        INT_TYPE count = key_buff_ptr_global[k].load(std::memory_order_relaxed);
        for (INT_TYPE c = 0; c < count; ++c) {
            key_array[idx++] = k;
        }
    }

/*  Send largest key value to next processor  */
    if( my_rank > 0 )
        lci::post_recv_x(my_rank-1, &k, 1 * sizeof(INT_TYPE), 1000, sync).device(devices[0]).allow_done(false)();
    if( my_rank < comm_size-1 ) {
        last_local_key = (idx == 0) ? idx : (idx - 1);
        while (lci::post_send_x(my_rank + 1, &key_array[last_local_key], 1 * sizeof(INT_TYPE), 1000, sync_send).device(devices[0]).allow_done(false)().is_retry()) {
            lci::progress_x().device(devices[0])();
        }
        lci::sync_wait_x(sync_send, nullptr).device(devices[0])();
    }
    if( my_rank > 0 )
        lci::sync_wait_x(sync, nullptr).device(devices[0])();

    free_comp(&sync);

/*  Confirm that neighbor's greatest key value
    is not greater than my least key value       */
    j = 0;
    if( my_rank > 0 && total_local_keys > 0 )
        if( k > key_array[0] )
            j++;

/*  Confirm keys correctly sorted: count incorrectly sorted keys, if any */
    #pragma omp parallel for schedule(static) reduction(+:j)
    for( i=1; i<total_local_keys; i++ )
        if( key_array[i-1] > key_array[i] )
            j++;

    if( j != 0 )
    {
        printf( "Processor %d:  Full_verify: number of keys out of sort: %d\n",
                my_rank, j );
    }
    else
        passed_verification++;

    TIMER_STOP( T_VERIFY );

}

// /*****************************************************************/
// /*************        SUM_OP FOR REDUCE           ****************/
// /*****************************************************************/

void sum_op_int(const void* left, const void* right, void* dst, size_t n)
{
    const INT_TYPE* left_ = static_cast<const INT_TYPE*>(left);
    const INT_TYPE* right_ = static_cast<const INT_TYPE*>(right);
    INT_TYPE* dst_ = static_cast<INT_TYPE*>(dst);
    for (size_t i = 0; i < n; ++i) {
        dst_[i] = left_[i] + right_[i];
    }
}

void sum_op_double(const void* left, const void* right, void* dst, size_t n)
{
    const double* left_ = static_cast<const double*>(left);
    const double* right_ = static_cast<const double*>(right);
    double* dst_ = static_cast<double*>(dst);
    for (size_t i = 0; i < n; ++i) {
        dst_[i] = left_[i] + right_[i];
    }
}

// /*****************************************************************/
// /*************        MAX_OP FOR REDUCE           ****************/
// /*****************************************************************/

void max_op(const void* left, const void* right, void* dst, size_t n)
{
    const double* left_ = static_cast<const double*>(left);
    const double* right_ = static_cast<const double*>(right);
    double* dst_ = static_cast<double*>(dst);
    for (size_t i = 0; i < n; ++i) {
        dst_[i] = (left_[i] > right_[i]) ? left_[i] : right_[i];
    }
}

// /*****************************************************************/
// /*************        MIN_OP FOR REDUCE           ****************/
// /*****************************************************************/

void min_op(const void* left, const void* right, void* dst, size_t n)
{
    const double* left_ = static_cast<const double*>(left);
    const double* right_ = static_cast<const double*>(right);
    double* dst_ = static_cast<double*>(dst);
    for (size_t i = 0; i < n; ++i) {
        dst_[i] = (left_[i] < right_[i]) ? left_[i] : right_[i];
    }
}

// /*****************************************************************/
// /*************        ALLOCATE DEVICES            ****************/
// /*****************************************************************/
int get_num_threads_per_device() {
    const char* env_val = getenv("NUM_THREADS_PER_DEVICE");
    int to_return = 1;
    if (env_val) {
        to_return = atoi(env_val);
        if (to_return <= 0 || to_return > omp_get_max_threads()) {
            if (my_rank == 0) fprintf(stderr, "[Warning] Invalid NUM_THREADS_PER_DEVICE value %d, using 1 instead\n", to_return);
            return 1;
        }
    } else {
        if (my_rank == 0) fprintf(stderr, "[Warning] NUM_THREADS_PER_DEVICE not set, using 1\n");
        return 1;
    }
    return to_return;
}

void allocate_devices() {
    // Allocate a device for each 2 threads, plus 1 main device
    NUM_THREADS_PER_PROC = omp_get_max_threads();
    NUM_THREADS_PER_DEVICE = get_num_threads_per_device();
    NUM_DEVICES = (NUM_THREADS_PER_PROC / NUM_THREADS_PER_DEVICE);

    size_t npackets = lci::get_default_packet_pool().get_attr_npackets();
    size_t max_nrecvs_per_device = std::min(npackets / 8 / NUM_DEVICES, 4096UL);
    size_t max_nsends_per_device = std::min(npackets / 4 / lci::get_rank_n() / NUM_DEVICES, 64UL);
    max_nsends_per_device = std::max(max_nsends_per_device, 4UL);

    for (int i = 0; i < NUM_DEVICES; ++i) {
        devices.push_back(lci::alloc_device_x().net_max_sends(max_nsends_per_device).net_max_recvs(max_nrecvs_per_device)());
    }
}

lci::device_t get_device_for_thread(int thread_id) {
    return devices[thread_id % NUM_DEVICES];
}

void free_devices() {
    for (auto& device : devices) {
        lci::free_device(&device);
    }
    devices.clear();
}

// /*****************************************************************/
// /*************             R  A  N  K             ****************/
// /*****************************************************************/

/*
 * One timed iteration of the sorting. Workflow:
 * bin keys into buckets (Step 2), greedily assign buckets to processes
 * (Step 3), redistribute keys via active messages (Step 4), then prefix-sum
 * the resulting key-frequency array into ranks (Step 5).
 */
void rank( int iteration )
{
    INT_TYPE    i;

    INT_TYPE    shift = MAX_KEY_LOG_2 - NUM_BUCKETS_LOG_2;
    INT_TYPE    key;
    KEY_TYPE    j, m;
    INT_TYPE    expected_recv_count;
    INT_TYPE    min_key_val, max_key_val;

    TIMER_START( T_RANK );
    TIMER_START( T_RANK_1 );

/*  Iteration alteration of keys */
    if(my_rank == 0 )
    {
      key_array[iteration] = iteration;
      key_array[iteration+MAX_ITERATIONS] = MAX_KEY - iteration;
    }

/*  Initialize */
    #pragma omp parallel for schedule(static)
    for( i=0; i<NUM_BUCKETS+TEST_ARRAY_SIZE; i++ )
    {
        bucket_size[i] = 0;
        bucket_size_totals[i] = 0;
        process_bucket_distrib_ptr1[i] = 0;
        process_bucket_distrib_ptr2[i] = 0;
        bucket_i_to_process_ranks[i] = 0;
    }

/*  Determine where the partial verify test keys are, load into  */
/*  top of array bucket_size                                     */
    for( i=0; i<TEST_ARRAY_SIZE; i++ )
        if( (test_index_array[i]/num_keys) == my_rank )
            bucket_size[NUM_BUCKETS+i] =
                          key_array[test_index_array[i] % num_keys];

    TIMER_START( T_RANK_1_1 );

/*  Step 2: count keys into local buckets; the global reduce/broadcast
    below completes the paper's global bucket-sizing stage.            */
    is_lci::count_local_buckets(
        key_array, num_keys, shift, bucket_size, NUM_BUCKETS);

    TIMER_STOP( T_RANK_1_1 );
    TIMER_STOP( T_RANK_1 );
    TIMER_STOP( T_RANK );

    TIMER_START( T_RCOMM );

/*  Complete Step 2 by aggregating and sharing the global bucket sizes. */
    lci::reduce_x(bucket_size,
                  bucket_size_totals,
                  NUM_BUCKETS + TEST_ARRAY_SIZE,
                  sizeof(INT_TYPE),
                  sum_op_int,
                  0).device(devices[0])();
    lci::broadcast_x(bucket_size_totals,
                     (NUM_BUCKETS + TEST_ARRAY_SIZE) * sizeof(INT_TYPE),
                     0).device(devices[0])();

    TIMER_STOP( T_RCOMM );

    TIMER_START( T_RANK );
    TIMER_START( T_RANK_2 );

/*  Step 3: greedily assign buckets to processes for coarse load
    balancing and derive the local key interval for this rank.      */
    is_lci::BucketPlan bucket_plan = is_lci::build_bucket_plan(
        bucket_size,
        bucket_size_totals,
        bucket_i_to_process_ranks,
        process_bucket_distrib_ptr1,
        process_bucket_distrib_ptr2,
        comm_size,
        my_rank,
        NUM_BUCKETS,
        shift,
        num_keys);

    expected_recv_count = bucket_plan.expected_recv_count;
    min_key_val = bucket_plan.min_key_value;
    max_key_val = bucket_plan.max_key_value;
    m = bucket_plan.lesser_key_count;
    j = bucket_plan.local_key_count;

/*  Clear the work array */
    #pragma omp parallel for schedule(static)
    for( i=0; i<max_key_val-min_key_val+1; i++ ) {
        key_buff1[i].store(0, std::memory_order_relaxed);
        cumulative_key_buff_ptr[i] = 0;
    }

/*  Shift the work array backwards so local rank updates can index by key. */
    key_buff_ptr = key_buff1 - min_key_val;
    is_lci::reset_redistributor_iteration(&redistributor_runtime, key_buff_ptr);

    lci::barrier_x().device(devices[0])();

    TIMER_STOP( T_RANK_2 );
    TIMER_STOP( T_RANK );

    TIMER_START( T_RCOMM );
    TIMER_START( T_ALLTOALL);
#ifdef A2A_TL_TIMERS
    a2atl::init(omp_get_max_threads());
#endif

/*  Step 4: redistribute keys with fine-grained active messages and tally
    arrivals into the local frequency histogram.                         */
    is_lci::RedistributorOptions redistributor_options{
        use_upacket == 1,
        use_loopback == 1,
        msg_batch_size,
    };
    is_lci::redistribute_keys(key_array,
                              num_keys,
                              bucket_i_to_process_ranks,
                              shift,
                              expected_recv_count,
                              comm_size,
                              my_rank,
                              devices,
                              redistributor_options,
                              &redistributor_runtime);

    TIMER_STOP( T_ALLTOALL );
    TIMER_STOP( T_RCOMM );

    #ifdef A2A_TL_TIMERS
    a2atl::stamp_wait_total();
    if (my_rank == 0) {
        a2atl::print_per_thread(iteration, my_rank);
        a2atl::print_minmax(iteration, my_rank);
    }
    #endif

    TIMER_START( T_RANK );
    TIMER_START( T_RANK_3 );

/*  Step 5: prefix-sum the local key-frequency array into final ranks.
    The lesser-key total m remains part of partial verification only.   */
    KEY_TYPE* cumulative = cumulative_key_buff_ptr - min_key_val;
    is_lci::compute_local_ranks(key_buff_ptr,
                                cumulative,
                                min_key_val,
                                max_key_val);

/* This is the partial verify test section */
/* Observe that test_rank_array vals are   */
/* shifted differently for different cases */
    for( i=0; i<TEST_ARRAY_SIZE; i++ )
    {
        k = bucket_size_totals[i+NUM_BUCKETS];    /* Keys were hidden here */
        if( min_key_val <= k  &&  k <= max_key_val )
        {
            /* Add the total of lesser keys, m, here */
            KEY_TYPE key_rank = cumulative[k-1] + m;
            KEY_TYPE test_rank = test_rank_array[i];
            int failed = 0;

            switch( CLASS )
            {
                case 'S':
                    if( i <= 2 )
                        test_rank += iteration;
                    else
                        test_rank -= iteration;
                    break;
                case 'W':
                    if( i < 2 )
                        test_rank += iteration - 2;
                    else
                        test_rank -= iteration;
                    break;
                case 'A':
                    if( i <= 2 )
                        test_rank += iteration - 1;
                    else
                        test_rank -= iteration - 1;
                    break;
                case 'B':
                    if( i == 1 || i == 2 || i == 4 )
                        test_rank += iteration;
                    else
                        test_rank -= iteration;
                    break;
                case 'C':
                    if( i <= 2 )
                        test_rank += iteration;
                    else
                        test_rank -= iteration;
                    break;
                case 'D':
                    if( i < 2 )
                        test_rank += iteration;
                    else
                        test_rank -= iteration;
                    break;
                case 'E':
                    if( i < 2 )
                        test_rank += iteration - 2;
                    else if( i == 2 )
                    {
                        test_rank += iteration - 2;
                        if (iteration > 4)
                            test_rank -= 2;
                        else if (iteration > 2)
                            test_rank -= 1;
                    }
                    else
                        test_rank -= iteration - 2;
                    break;
            }
            if( key_rank != test_rank )
                failed = 1;
            else
                passed_verification++;
            if( failed == 1 )
                printf( "Failed partial verification: "
                        "iteration %d, processor %d, test key %d, key rank %ld\n",
                         iteration, my_rank, (int)i, (long)key_rank );
        }
    }

    TIMER_STOP( T_RANK_3 );
    TIMER_STOP( T_RANK );

    #ifdef A2A_TL_TIMERS
        a2atl::print_per_process(iteration, my_rank, (long long)j, timer_read(T_RANK));
        #pragma omp parallel
        {
            a2atl::reset_thread_local(omp_get_max_threads());
        }
    #endif

/*  Make copies of rank info for use by full_verify: these variables
    in rank are local; making them global slows down the code, probably
    since they cannot be made register by compiler                        */

    if( iteration == MAX_ITERATIONS )
    {
        key_buff_ptr_global = key_buff_ptr;
        min_key_val_global = min_key_val;
        max_key_val_global = max_key_val;
        total_local_keys    = j;
    }

}

/*****************************************************************/
/*************             M  A  I  N             ****************/
/*****************************************************************/

int main( int argc, char **argv )
{

    int             i, iteration, itemp, active;

    double          timecounter, maxtime;

/*  Initialize MPI and LCI */
    setvbuf(stderr, nullptr, _IONBF, 0);
    lci::g_runtime_init_x().alloc_default_device(false)();
    my_rank = lci::get_rank_me();
    np_total = lci::get_rank_n();
    allocate_devices();
    lci::barrier_x().device(devices[0])();

/*  Check to see whether total number of processes is within bounds.
    This could in principle be checked in setparams.c, but it is more
    convenient to do it here                                               */
    if( np_total < MIN_PROCS || np_total > MAX_PROCS)
    {
       if( my_rank == 0 )
           printf( "\n ERROR: number of processes %d not within range %d-%d"
                   "\n Exiting program!\n\n", np_total, MIN_PROCS, MAX_PROCS);
       free_devices();
       lci::g_runtime_fina();
       exit( 1 );
    }

/*  comm_size needs to be power of two */
    for (comm_size = 1; comm_size < np_total; comm_size *= 2);
    if (comm_size > np_total) comm_size /= 2;

/*  If the actual number of processes doesn't agree with comm_size,
    check if excess ranks need to be masked */
    active = 1;
    if( comm_size != np_total )
    {
        /* check if NPB_NPROCS_STRICT is set */
        if( my_rank == 0 ) {
            char *ep = getenv("NPB_NPROCS_STRICT");
            if (ep && *ep) {
               if (strchr("nNfF-", *ep) || strcmp(ep, "0") == 0)
                  active = 0;
               else if (strcmp(ep, "off") == 0 || strcmp(ep, "OFF") == 0)
                  active = 0;
            }
        }
        lci::broadcast_x(&active, 1 * sizeof(int), 0).device(devices[0])();

        /* abort if a strict NPROCS enforcement is required */
        if (active) {
            if( my_rank == 0 )
               fprintf(stderr, "\n ERROR: Number of processes (%d)"
                       " is not a power of two (%d?)\n"
                       " Exiting program!\n\n", np_total, comm_size );
            free_devices();
            lci::g_runtime_fina();
            exit( 1 );
        }

        /* mark excess ranks as inactive */
        active = ( my_rank >= comm_size )? 0 : 1;
    }

    if (!active) {
        free_devices();
        lci::g_runtime_fina();
        exit( 0 );
    }

/*  Initialize the verification arrays if a valid class */
    #pragma omp parallel for schedule(static)
    for( i=0; i<TEST_ARRAY_SIZE; i++ )
        switch( CLASS )
        {
            case 'S':
                test_index_array[i] = S_test_index_array[i];
                test_rank_array[i]  = S_test_rank_array[i];
                break;
            case 'A':
                test_index_array[i] = A_test_index_array[i];
                test_rank_array[i]  = A_test_rank_array[i];
                break;
            case 'W':
                test_index_array[i] = W_test_index_array[i];
                test_rank_array[i]  = W_test_rank_array[i];
                break;
            case 'B':
                test_index_array[i] = B_test_index_array[i];
                test_rank_array[i]  = B_test_rank_array[i];
                break;
            case 'C':
                test_index_array[i] = C_test_index_array[i];
                test_rank_array[i]  = C_test_rank_array[i];
                break;
            case 'D':
                test_index_array[i] = D_test_index_array[i];
                test_rank_array[i]  = D_test_rank_array[i];
                break;
            case 'E':
                test_index_array[i] = E_test_index_array[i];
                test_rank_array[i]  = E_test_rank_array[i];
                break;
        };

/*  Printout initial NPB info */
    if( my_rank == 0 )
    {
        printf( "\n\n NAS Parallel Benchmarks 3.4 -- IS Benchmark\n\n" );
        printf( " Size:  %ld  (class %c)\n", (long)TOTAL_KEYS*MIN_PROCS, CLASS );
        printf( " Iterations:   %d\n", MAX_ITERATIONS );
        printf( " Total number of processes:  %d\n", np_total );
        if ( comm_size != np_total )
            printf( " WARNING: Number of processes"
                    " is not a power of two (%d active)\n", comm_size );
        use_upacket = check_use_upacket_flag();
        use_loopback = check_loopback_flag();
        timeron = check_timer_flag();

        if (use_upacket)
            printf( " Using upacket for buffer management\n" );
        else
            printf( " Using malloc/free for buffer management\n" );
        if (use_loopback)
            printf( " Loopback optimization: ENABLED\n" );
        else
            printf( " Loopback optimization: DISABLED\n" );
    }

    lci::broadcast_x(&use_upacket, 1 * sizeof(int), 0).device(devices[0])();
    lci::broadcast_x(&use_loopback, 1 * sizeof(int), 0).device(devices[0])();
    lci::broadcast_x(&timeron, 1 * sizeof(int), 0).device(devices[0])();

#ifdef  TIMING_ENABLED
    #pragma omp parallel for schedule(static)
    for( i=1; i<=T_LAST; i++ ) timer_clear( i );
#endif

/*  allocate space for work arrays */
    alloc_space();

    is_lci::RedistributorOptions redistributor_options{
        use_upacket == 1,
        use_loopback == 1,
        msg_batch_size,
    };
    is_lci::allocate_fallback_buffers(&redistributor_runtime,
                                      comm_size,
                                      omp_get_max_threads(),
                                      redistributor_options);

/*  Generate random number sequence and subsequent keys on all procs */
    is_lci::generate_keys(
        key_array,
        num_keys,
        is_lci::problem_config().max_key,
        is_lci::find_my_seed(my_rank,
                             comm_size,
                             4 * (long)TOTAL_KEYS * MIN_PROCS,
                             314159265.00,
                             1220703125.00),
        1220703125.00);

/*  Initialize LCI active message properties */
    is_lci::initialize_redistributor(&redistributor_runtime,
                                     redistributor_options);
    lci::barrier_x().device(devices[0])();

/*  Do one interation for free (i.e., untimed) to guarantee initialization of
    all data and code pages and respective tables */
    rank( 1 );

/*  Start verification counter */
    passed_verification = 0;

    if( my_rank == 0 && CLASS != 'S' ) printf( "\n   iteration\n" );

/*  Initialize timer  */
    timer_clear( 0 );

/*  Initialize separate communication, computation timing */
#ifdef  TIMING_ENABLED
    #pragma omp parallel for schedule(static)
    for( i=1; i<=T_LAST; i++ ) timer_clear( i );
#endif

/*  Start timer  */
    timer_start( 0 );

/*  This is the main iteration */
    for( iteration=1; iteration<=MAX_ITERATIONS; iteration++ )
    {
        if( my_rank == 0 && CLASS != 'S' ) printf( "        %d\n", iteration );
        lci::barrier_x().device(devices[0])();
        rank( iteration );
    }

/*  Stop timer, obtain time for processors */
    timer_stop( 0 );

    timecounter = timer_read( 0 );

/*  End of timing, obtain maximum time of all processors */
    lci::reduce_x(&timecounter, &maxtime, 1, sizeof(double), max_op, 0).device(devices[0])();

/*  This tests that keys are in sequence: sorting of last ranked key seq
    occurs here, but is an untimed operation                             */
    full_verify();

/*  Obtain verification counter sum */
    itemp = passed_verification;
    lci::reduce_x(&itemp, &passed_verification, 1, sizeof(int), sum_op_int, 0).device(devices[0])();

    free_space();

/*  The final printout  */
    if( my_rank == 0 )
    {
        /* Partial verification (5 tests/iteration) + full verification (per rank) */
        int expected_verification = 5*MAX_ITERATIONS + comm_size;
        if( passed_verification != expected_verification )
            passed_verification = 0;
        c_print_results( "IS",
                         CLASS,
                         (int)(TOTAL_KEYS),
                         MIN_PROCS,
                         0,
                         MAX_ITERATIONS,
                         comm_size,
                         np_total,
                         maxtime,
                         ((double) (MAX_ITERATIONS)*TOTAL_KEYS*MIN_PROCS)
                                                      /maxtime/1000000.,
                         "keys ranked",
                         passed_verification,
                         NPBVERSION,
                         COMPILETIME,
                         MPICC,
                         CLINK,
                         CMPI_LIB,
                         CMPI_INC,
                         CFLAGS,
                         CLINKFLAGS );
    }

#ifdef  TIMING_ENABLED
    if (timeron)
    {
        double    t1[T_LAST+1], tmin[T_LAST+1], tsum[T_LAST+1], tmax[T_LAST+1];
        char      t_recs[T_LAST+1][9];

        #pragma omp parallel for schedule(static)
        for( i=0; i<=T_LAST; i++ )
            t1[i] = timer_read( i );

        lci::reduce_x(t1, tmin, T_LAST+1, sizeof(double), min_op, 0).device(devices[0])();
        lci::reduce_x(t1, tsum, T_LAST+1, sizeof(double), sum_op_double, 0).device(devices[0])();
        lci::reduce_x(t1, tmax, T_LAST+1, sizeof(double), max_op, 0).device(devices[0])();

        if( my_rank == 0 )
        {
            strcpy( t_recs[T_TOTAL],  "total" );
            strcpy( t_recs[T_RANK],   "rcomp" );
            strcpy( t_recs[T_RCOMM],  "rcomm" );
            strcpy( t_recs[T_VERIFY], "verify");
            strcpy( t_recs[T_ALLTOALL],"atallv" );
            strcpy( t_recs[T_RANK_1], "rcomp1" );
            strcpy( t_recs[T_RANK_2], "rcomp2" );
            strcpy( t_recs[T_RANK_3], "rcomp3" );
            strcpy( t_recs[T_RANK_1_1], "rcomp1.1" );
            printf( " nprocs = %6d     ", comm_size);
            printf( "     minimum     maximum     average\n" );
            for( i=0; i<=T_LAST; i++ )
            {
                printf( " timer %2d (%-8s):  %10.4f  %10.4f  %10.4f\n",
                        i+1, t_recs[i], tmin[i], tmax[i],
                        tsum[i]/((double) comm_size) );
            }
            printf( "\n" );
        }
    }
#endif

    is_lci::finalize_redistributor(&redistributor_runtime);
    free_devices();
    lci::g_runtime_fina();

    return 0;
         /**************************/
}        /*  E N D  P R O G R A M  */
         /**************************/
