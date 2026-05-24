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
 *    Step 1  create_seq()  Generate Gaussian-distributed keys.
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

#include <lci.hpp>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <omp.h>
#include <map>
#include <atomic>

/******************/
/* default values */
/******************/
#ifndef CLASS
#define CLASS 'S'
#define NUM_PROCS            1
#endif
#define MIN_PROCS            1
#define ONE                  1

/*************/
/*  CLASS S  */
/*************/
#if CLASS == 'S'
#define  TOTAL_KEYS_LOG_2    16
#define  MAX_KEY_LOG_2       11
#define  NUM_BUCKETS_LOG_2   9
#endif

/*************/
/*  CLASS W  */
/*************/
#if CLASS == 'W'
#define  TOTAL_KEYS_LOG_2    20
#define  MAX_KEY_LOG_2       16
#define  NUM_BUCKETS_LOG_2   10
#endif

/*************/
/*  CLASS A  */
/*************/
#if CLASS == 'A'
#define  TOTAL_KEYS_LOG_2    23
#define  MAX_KEY_LOG_2       19
#define  NUM_BUCKETS_LOG_2   10
#endif

/*************/
/*  CLASS B  */
/*************/
#if CLASS == 'B'
#define  TOTAL_KEYS_LOG_2    25
#define  MAX_KEY_LOG_2       21
#define  NUM_BUCKETS_LOG_2   10
#endif

/*************/
/*  CLASS C  */
/*************/
#if CLASS == 'C'
#define  TOTAL_KEYS_LOG_2    27
#define  MAX_KEY_LOG_2       23
#define  NUM_BUCKETS_LOG_2   10
#endif

/*************/
/*  CLASS D  */
/*************/
#if CLASS == 'D'
#define  TOTAL_KEYS_LOG_2    29     /* 2^31 */
#define  MAX_KEY_LOG_2       27
#define  NUM_BUCKETS_LOG_2   10
#undef   MIN_PROCS
#define  MIN_PROCS           4
#endif

/*************/
/*  CLASS E  */
/*************/
#if CLASS == 'E'
#define  TOTAL_KEYS_LOG_2    29     /* 2^35 */
#define  MAX_KEY_LOG_2       31
#define  NUM_BUCKETS_LOG_2   10
#undef   MIN_PROCS
#define  MIN_PROCS           64
#undef   ONE
#define  ONE                 1L
#endif

/*******************************************************************
 * Defining MIN_PROCS is to avoid integer overflow for large problem
 * sizes without using a larger integer type, such as long int.
 * The actual total keys = TOTAL_KEYS * MIN_PROCS
 *******************************************************************/
#define  TOTAL_KEYS          (1 << TOTAL_KEYS_LOG_2)

#define  MAX_KEY             (ONE << MAX_KEY_LOG_2)
#define  NUM_BUCKETS         (1 << NUM_BUCKETS_LOG_2)

/*****************************************************************/
/* NOTE: THIS CODE CANNOT BE RUN ON ARBITRARILY LARGE NUMBERS OF */
/* PROCESSORS. THE LARGEST VERIFIED NUMBER IS 1024. INCREASE     */
/* MAX_PROCS AT YOUR PERIL                                       */
/*****************************************************************/
#if CLASS == 'S'
#define  MAX_PROCS           128
#else
#define  MAX_PROCS           4096
#endif

#define  MAX_ITERATIONS      10
#define  TEST_ARRAY_SIZE     5

/*****************************************************************/
/* Number of keys batched into one active message (Step 4).      */
/* Capped to the eager-protocol limit; set in alloc_space().     */
/*****************************************************************/
int msg_batch_size;
int NUM_THREADS_PER_PROC;
int NUM_DEVICES;
int NUM_THREADS_PER_DEVICE;
// Pre-allocated buffer for get_upacket_blocking when use_upacket is false
void* preallocated_buffer = nullptr;

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

/*************************************/
/* Typedef: if necessary, change the */
/* size of int here by changing the  */
/* int type to, say, long            */
/*************************************/
// Note: KEY_TYPE is the type of variables used for counting keys/ranks
typedef  int  INT_TYPE;
#if CLASS == 'D' || CLASS == 'E'
typedef  long KEY_TYPE;
#else
typedef  int  KEY_TYPE;
#endif
#define MP_KEY_TYPE MPI_INT

/********************/
/* MPI properties:  */
/********************/
int      my_rank, np_total,
         comm_size;

/********************/
/* LCI properties:  */
/********************/
lci::comp_t send_bucket_handler;
lci::rcomp_t send_bucket_rcomp;
lci::comp_t send_counter;
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

/***********************/
/* function prototypes */
/***********************/
double	randlc( double *X, double *A );

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
#include <iostream>
#include <unistd.h>

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

   /* Pre-allocate buffer for get_upacket_blocking when use_upacket is false */
   if (!use_upacket && preallocated_buffer == nullptr) {
       preallocated_buffer = malloc(msg_batch_size * sizeof(INT_TYPE) * comm_size * omp_get_max_threads());
   }

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

/*
 *    FUNCTION RANDLC (X, A)
 *
 *  This routine returns a uniform pseudorandom double precision number in the
 *  range (0, 1) by using the linear congruential generator
 *
 *  x_{k+1} = a x_k  (mod 2^46)
 *
 *  where 0 < x_k < 2^46 and 0 < a < 2^46.  This scheme generates 2^44 numbers
 *  before repeating.  The argument A is the same as 'a' in the above formula,
 *  and X is the same as x_0.  A and X must be odd double precision integers
 *  in the range (1, 2^46).  The returned value RANDLC is normalized to be
 *  between 0 and 1, i.e. RANDLC = 2^(-46) * x_1.  X is updated to contain
 *  the new seed x_1, so that subsequent calls to RANDLC using the same
 *  arguments will generate a continuous sequence.
 *
 *  This routine should produce the same results on any computer with at least
 *  48 mantissa bits in double precision floating point data.  On Cray systems,
 *  double precision should be disabled.
 *
 *  David H. Bailey     October 26, 1990
 *
 *     IMPLICIT DOUBLE PRECISION (A-H, O-Z)
 *     SAVE KS, R23, R46, T23, T46
 *     DATA KS/0/
 *
 *  If this is the first call to RANDLC, compute R23 = 2 ^ -23, R46 = 2 ^ -46,
 *  T23 = 2 ^ 23, and T46 = 2 ^ 46.  These are computed in loops, rather than
 *  by merely using the ** operator, in order to insure that the results are
 *  exact on all systems.  This code assumes that 0.5D0 is represented exactly.
 */

/*****************************************************************/
/*************           R  A  N  D  L  C             ************/
/*************                                        ************/
/*************    portable random number generator    ************/
/*****************************************************************/

double	randlc( double *X, double *A )
{
      static int        KS=0;
      static double	R23, R46, T23, T46;
      double		T1, T2, T3, T4;
      double		A1;
      double		A2;
      double		X1;
      double		X2;
      double		Z;
      int     		i, j;

      if (KS == 0)
      {
        R23 = 1.0;
        R46 = 1.0;
        T23 = 1.0;
        T46 = 1.0;

        for (i=1; i<=23; i++)
        {
          R23 = 0.50 * R23;
          T23 = 2.0 * T23;
        }
        for (i=1; i<=46; i++)
        {
          R46 = 0.50 * R46;
          T46 = 2.0 * T46;
        }
        KS = 1;
      }

/*  Break A into two parts such that A = 2^23 * A1 + A2 and set X = N.  */

      T1 = R23 * *A;
      j  = T1;
      A1 = j;
      A2 = *A - T23 * A1;

/*  Break X into two parts such that X = 2^23 * X1 + X2, compute
    Z = A1 * X2 + A2 * X1  (mod 2^23), and then
    X = 2^23 * Z + A2 * X2  (mod 2^46).                            */

      T1 = R23 * *X;
      j  = T1;
      X1 = j;
      X2 = *X - T23 * X1;
      T1 = A1 * X2 + A2 * X1;

      j  = R23 * T1;
      T2 = j;
      Z = T1 - T23 * T2;
      T3 = T23 * Z + A2 * X2;
      j  = R46 * T3;
      T4 = j;
      *X = T3 - T46 * T4;
      return(R46 * *X);
}

/*****************************************************************/
/************   F  I  N  D  _  M  Y  _  S  E  E  D    ************/
/************                                         ************/
/************ returns parallel random number seq seed ************/
/*****************************************************************/

/*
 * Create a random number sequence of total length nn residing
 * on np number of processors.  Each processor will therefore have a
 * subsequence of length nn/np.  This routine returns that random
 * number which is the first random number for the subsequence belonging
 * to processor rank kn, and which is used as seed for proc kn ran # gen.
 */

double   find_my_seed( int  kn,       /* my processor rank, 0<=kn<=num procs */
                       int  np,       /* np = num procs                      */
                       long nn,       /* total num of ran numbers, all procs */
                       double s,      /* Ran num seed, for ex.: 314159265.00 */
                       double a )     /* Ran num gen mult, try 1220703125.00 */
{

  long   i;

  double t1,t2,t3,an;
  long   mq,nq,kk,ik;

      nq = nn / np;

      for( mq=0; nq>1; mq++,nq/=2 )
          ;

      t1 = a;

      for( i=1; i<=mq; i++ )
        t2 = randlc( &t1, &t1 );

      an = t1;

      kk = kn;
      t1 = s;
      t2 = an;

      for( i=1; i<=100; i++ )
      {
        ik = kk / 2;
        if( 2 * ik !=  kk )
            t3 = randlc( &t1, &t2 );
        if( ik == 0 )
            break;
        t3 = randlc( &t2, &t2 );
        kk = ik;
      }

      return( t1 );

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
/*************      C  R  E  A  T  E  _  S  E  Q      ************/
/*****************************************************************/

/*
 * Step 1: generate this rank's keys with the NPB randlc() generator. Each
 * key is the sum of four uniform random values, giving a Gaussian-like
 * distribution over [0, MAX_KEY).
 */
void	create_seq( double seed, double a )
{
	double x;
	int    i, k;

	k = MAX_KEY/4;

	for (i=0; i<num_keys; i++)
	{
		x = randlc(&seed, &a);
		x += randlc(&seed, &a);
		x += randlc(&seed, &a);
		x += randlc(&seed, &a);

		key_array[i] = k*x;
	}
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
// /*************      ACTIVE MESSAGE HANDLER        ****************/
// /*****************************************************************/

/* Keys this process has received and tallied so far this iteration; the     */
/* Step 4 redistribution loop spins on it to know when all expected keys     */
/* have arrived. Cache-line padded to avoid false sharing.                   */
alignas(64) std::atomic<size_t> global_recv_count{0};
char padding[64 - sizeof(global_recv_count)];

const size_t KEY_SIZE = sizeof(INT_TYPE);

/*
 * Tally a batch of received keys into the key-frequency array (key_buff_ptr):
 * the slot for value v counts how many keys equal v. This frequency array is
 * what Step 5 prefix-sums into ranks. Runs on every thread, so increments
 * are atomic.
 */
void handle_received_keys(const void* src, size_t num_of_keys) {
    const INT_TYPE* keys = static_cast<const INT_TYPE*>(src);
    for (size_t i = 0; i < num_of_keys; ++i) {
        key_buff_ptr[keys[i]].fetch_add(1, std::memory_order_relaxed);
    }
    global_recv_count.fetch_add(num_of_keys, std::memory_order_relaxed);
}

/* LCI active-message handler: invoked on the receiver when a batch of keys   */
/* arrives, tallies them into the key-frequency array, and recycles the      */
/* packet when zero-copy upackets are in use.                                */
void am_handler(lci::status_t status)
{
#ifdef A2A_TL_TIMERS
    TL_STEP_START(A2A_AM_COPY);
#endif
    handle_received_keys(status.get_buffer(), status.get_size() / KEY_SIZE);
#ifdef A2A_TL_TIMERS
    TL_STEP_STOP(A2A_AM_COPY);
    TL_ADD_BYTES(A2A_AM_COPY, status.get_size());
#endif
    if (use_upacket) {
        lci::put_upacket(status.get_buffer());
    }
}

static inline void* get_upacket_blocking(lci::device_t device, int dest_rank) {
    if (use_upacket) {
        void* upacket = lci::get_upacket();
        while (upacket == nullptr) {
            upacket = lci::get_upacket();
            lci::progress_x().device(device)();
        }
        return upacket;
    } else {
        // Return pre-allocated buffer when upacket is disabled
        // Each rank allocates its own buffer pool: comm_size * num_threads buffers
        // Index: (thread_id * comm_size + dest_rank) * msg_batch_size
        int thread_id = omp_get_thread_num();
        int buffer_index = (thread_id * comm_size + dest_rank) * msg_batch_size;
        return static_cast<void*>(static_cast<INT_TYPE*>(preallocated_buffer) + buffer_index);
    }
}

/*
 * Per-destination send buffer (Step 4): a thread appends keys bound for one
 * destination process and flushes the whole buffer as a single active message
 * once it fills (msg_batch_size keys). Backed by either an LCI zero-copy
 * packet ("upacket") or a slot in preallocated_buffer.
 */
class SendBuffer {
  public:
    SendBuffer() = default;

    ~SendBuffer() {
        release();
    }

    void release() {
        buf_ = nullptr;
        buf_int_ = nullptr;
        size_ = 0;
    }

    void push(INT_TYPE key, lci::device_t device, int dest_rank) {
        if (!buf_) {
            buf_ = get_upacket_blocking(device, dest_rank);
            buf_int_ = static_cast<INT_TYPE*>(buf_);
        }
        buf_int_[size_++] = key;
    }

    void* data() const { return buf_; }
    size_t size() const { return static_cast<size_t>(size_); }
    size_t size_in_bytes() const { return static_cast<size_t>(size_) * KEY_SIZE; }
    bool empty() const { return size_ == 0; }

  private:
    void* buf_ = nullptr;
    INT_TYPE* buf_int_ = nullptr;
    int size_ = 0;
};

/* Flush one destination's batched keys as an active message, retrying until  */
/* LCI accepts it. Loopback optimization: if the destination is this same     */
/* process, invoke the handler directly instead of messaging ourselves.       */
void flush_send_buffer(std::vector<SendBuffer>& send_buffers, int dest_rank, lci::device_t device) {
    SendBuffer& send_buf = send_buffers[dest_rank];
    if (send_buf.empty()) return;

#ifdef A2A_TL_TIMERS
    TL_STEP_START(A2A_FLUSH_SEND);
#endif
    /* Loopback optimization: destination is the local process. */
    if (dest_rank == my_rank && use_loopback) {
#ifdef A2A_TL_TIMERS
        TL_STEP_START(A2A_SELF_COPY);
#endif
        handle_received_keys(send_buf.data(), send_buf.size());
        if (use_upacket) {
            lci::put_upacket(send_buf.data());
        }
#ifdef A2A_TL_TIMERS
        TL_STEP_STOP(A2A_SELF_COPY);
        TL_ADD_BYTES(A2A_SELF_COPY, send_buf.size_in_bytes());
#endif
    } else {
        lci::status_t status;
        do {
            status = lci::post_am_x(dest_rank, send_buf.data(), send_buf.size_in_bytes(), send_counter, send_bucket_rcomp)
                        .comp_semantic(lci::comp_semantic_t::network)
                        .device(device)();
            lci::progress_x().device(device)();
        } while (status.is_retry());
    }
#ifdef A2A_TL_TIMERS
    TL_STEP_STOP(A2A_FLUSH_SEND);
    TL_ADD_BYTES(A2A_FLUSH_SEND, send_buf.size_in_bytes());
#endif
    send_buf.release();
}

/* Route one key to its destination's send buffer, flushing that buffer as an */
/* active message once it reaches msg_batch_size keys.                        */
void send_key_to_processor(INT_TYPE key, int dest_rank,
                           std::vector<SendBuffer>& send_buffers,
                           lci::device_t device) {
    send_buffers[dest_rank].push(key, device, dest_rank);
    if (send_buffers[dest_rank].size() >= msg_batch_size) {
        flush_send_buffer(send_buffers, dest_rank, device);
    }
}

void flush_all_send_buffers(std::vector<SendBuffer>& send_buffers, lci::device_t device) {
    for (int i = 0; i < comm_size; ++i) {
        auto index = (my_rank + i) % comm_size; // start from self rank
        if (!send_buffers[index].empty()) {
            flush_send_buffer(send_buffers, index, device);
        }
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
    INT_TYPE    i, k;

    INT_TYPE    shift = MAX_KEY_LOG_2 - NUM_BUCKETS_LOG_2;
    INT_TYPE    key;
    KEY_TYPE    bucket_sum_accumulator, j, m;
    INT_TYPE    local_bucket_sum_accumulator;
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

    global_recv_count.store(0);
    lci::counter_set(send_counter, 0);

/*  Determine where the partial verify test keys are, load into  */
/*  top of array bucket_size                                     */
    for( i=0; i<TEST_ARRAY_SIZE; i++ )
        if( (test_index_array[i]/num_keys) == my_rank )
            bucket_size[NUM_BUCKETS+i] =
                          key_array[test_index_array[i] % num_keys];

    TIMER_START( T_RANK_1_1 );

/*  Step 2: organize keys into buckets -- count keys per local bucket    */
/*  (threads build private histograms, then merge; the global reduce     */
/*  below completes Step 2).                                             */
    #pragma omp parallel
    {
        int bucket_size_private[NUM_BUCKETS] = {0};
        #pragma omp for nowait
        for( i=0; i<num_keys; i++ ) {
            bucket_size_private[key_array[i] >> shift]++;
        }
        #pragma omp critical
        {
            for (int b = 0; b < NUM_BUCKETS; ++b) {
                bucket_size[b] += bucket_size_private[b];
            }
        }
    }

    TIMER_STOP( T_RANK_1_1 );
    TIMER_STOP( T_RANK_1 );
    TIMER_STOP( T_RANK );

    TIMER_START( T_RCOMM );

/*  Get the bucket size totals for the entire problem. These
    will be used to determine the redistribution of keys      */
    lci::reduce_x(bucket_size, bucket_size_totals, NUM_BUCKETS+TEST_ARRAY_SIZE, sizeof(INT_TYPE), sum_op_int, 0).device(devices[0])();
    lci::broadcast_x(bucket_size_totals, (NUM_BUCKETS+TEST_ARRAY_SIZE) * sizeof(INT_TYPE), 0).device(devices[0])();

    TIMER_STOP( T_RCOMM );

    TIMER_START( T_RANK );
    TIMER_START( T_RANK_2 );

/*  Step 3: greedily assign buckets to processes (coarse load balancing).
    Accumulate the bucket size totals until the running total surpasses
    NUM_KEYS (which is the average number of keys
    per processor).  Then all keys in these buckets go to processor 0.
    Continue accumulating again until surpassing 2*NUM_KEYS. All keys
    in these buckets go to processor 1, etc.  This algorithm guarantees
    that all processors have work ranking; no processors are left idle.
    The optimum number of buckets, however, does not result in as high
    a degree of load balancing (as even a distribution of keys as is
    possible) as is obtained from increasing the number of buckets, but
    more buckets results in more computation per processor so that the
    optimum number of buckets turns out to be 1024 for machines tested.
    Note that process_bucket_distrib_ptr1 and ..._ptr2 hold the bucket
    number of first and last bucket which each processor will have after
    the redistribution is done.                                          */

    bucket_sum_accumulator = 0;
    local_bucket_sum_accumulator = 0;
    process_bucket_distrib_ptr1[0] = 0;
    expected_recv_count = 0;
    INT_TYPE previous_bucket_sum_accumulator = 0;

    for( i=0, j=0; i<NUM_BUCKETS; i++ )
    {
        bucket_sum_accumulator       += bucket_size_totals[i];
        local_bucket_sum_accumulator += bucket_size[i];

        bucket_i_to_process_ranks[i] = j;  // map bucket index to processor rank

        if( bucket_sum_accumulator >= (j+1)*num_keys )
        {
            if ( j == my_rank ) {
                expected_recv_count = bucket_sum_accumulator - previous_bucket_sum_accumulator;
            }
            if( j != 0 )
            {
                process_bucket_distrib_ptr1[j] =
                                        process_bucket_distrib_ptr2[j-1]+1;
            }
            process_bucket_distrib_ptr2[j++] = i;
            local_bucket_sum_accumulator = 0;
            previous_bucket_sum_accumulator = bucket_sum_accumulator;
        }
    }

/*  When NUM_PROCS approaching NUM_BUCKETS, it is highly possible
    that the last few processors don't get any buckets.  So, we
    need to set counts properly in this case to avoid any fallouts.    */
    while( j < comm_size )
    {
        process_bucket_distrib_ptr1[j] = 1;
        j++;
    }

/*  The starting and ending bucket numbers on each processor are
    multiplied by the interval size of the buckets to obtain the
    smallest possible min and greatest possible max value of any
    key on each processor                                          */
    min_key_val = process_bucket_distrib_ptr1[my_rank] << shift;
    max_key_val = ((process_bucket_distrib_ptr2[my_rank] + 1) << shift)-1;

/*  Clear the work array */
    #pragma omp parallel for schedule(static)
    for( i=0; i<max_key_val-min_key_val+1; i++ ) {
        key_buff1[i].store(0, std::memory_order_relaxed);
        cumulative_key_buff_ptr[i] = 0;
    }

/*  Determine the total number of keys on all other
    processors holding keys of lesser value         */
    m = 0;
    for( k=0; k<my_rank; k++ )
        for( i= process_bucket_distrib_ptr1[k];
             i<=process_bucket_distrib_ptr2[k];
             i++ )
            m += bucket_size_totals[i]; /*  m has total # of lesser keys */

/*  Determine total number of keys on this processor */
    j = 0;
    for( i= process_bucket_distrib_ptr1[my_rank];
         i<=process_bucket_distrib_ptr2[my_rank];
         i++ )
        j += bucket_size_totals[i];     /* j has total # of local keys   */

/*  Ranking of all keys occurs in this section:                 */
/*  shift it backwards so no subtractions are necessary in loop */
    key_buff_ptr = key_buff1 - min_key_val;

    lci::barrier_x().device(devices[0])();

    TIMER_STOP( T_RANK_2 );
    TIMER_STOP( T_RANK );

    TIMER_START( T_RCOMM );
    TIMER_START( T_ALLTOALL);
#ifdef A2A_TL_TIMERS
    a2atl::init(omp_get_max_threads());
#endif

/*  Step 4: redistribute keys with fine-grained active messages. Each thread
    routes each of its keys to the process that owns the key's bucket, batching
    into per-destination send buffers and flushing them as active messages,
    then progresses LCI until this process has received all expected keys
    (tallied by handle_received_keys).                                         */
    #pragma omp parallel
    {
        int thread_id = omp_get_thread_num();
        std::vector<SendBuffer> send_buffers(comm_size);
        lci::device_t device = get_device_for_thread(thread_id);

        #pragma omp for nowait
        for (i=0; i<num_keys; i++) {
            auto dest_rank = bucket_i_to_process_ranks[key_array[i] >> shift];
            send_key_to_processor(key_array[i], dest_rank, send_buffers, device);
        }
        flush_all_send_buffers(send_buffers, device);

    #ifdef A2A_TL_TIMERS
        TL_STEP_START(A2A_PROGRESS_WAIT);
    #endif
        /* Pick up incoming active messages until every expected key has  */
        /* arrived and been tallied into the key-frequency array.         */
        while (global_recv_count.load() < expected_recv_count) {
            lci::progress_x().device(device)();
        }
    #ifdef A2A_TL_TIMERS
        TL_STEP_STOP(A2A_PROGRESS_WAIT);
        a2atl::publish_thread_stats(thread_id);
    #endif
    }

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

/*  Step 5: compute final key ranks by prefix-summing the key-frequency
    array. Successively add each key population, not forgetting the total of
    lesser keys, m.
    NOTE: Since the total of lesser keys would be subtracted later
    in verification, it is no longer added to the first key population
    here, but still needed during the partial verify test.  This is to
    ensure that 32-bit key_buff can still be used for class D.
    The scan is parallel: per-thread partial sums, an exclusive scan of
    those into offsets, then each thread scans its chunk from its offset.  */
    KEY_TYPE* cumulative = cumulative_key_buff_ptr - min_key_val;
    const INT_TYPE start_key = min_key_val;
    const INT_TYPE N = max_key_val - min_key_val + 1;

    std::vector<KEY_TYPE> partial_sums;

    #pragma omp parallel
    {
        int nthreads = omp_get_num_threads();
        int tid = omp_get_thread_num();

        #pragma omp single
        {
            partial_sums.resize(nthreads);
        }

        // Each thread calculates the sum of its local chunk
        KEY_TYPE local_sum = 0;
        #pragma omp for schedule(static) nowait
        for (INT_TYPE i = 0; i < N; i++) {
            local_sum += key_buff_ptr[start_key + i].load(std::memory_order_relaxed);
        }
        partial_sums[tid] = local_sum;

        #pragma omp barrier

        // One thread computes the exclusive prefix sum of the partial sums.
        // This gives each thread its starting offset.
        #pragma omp single
        {
            KEY_TYPE temp_sum = 0;
            for (int i = 0; i < nthreads; i++) {
                KEY_TYPE val = partial_sums[i];
                partial_sums[i] = temp_sum; // partial_sums[tid] now holds the offset
                temp_sum += val;
            }
        }
        // Barrier is implicit after 'single'

        // Each thread performs a sequential scan on its local chunk,
        // starting from the offset computed in the sequential pass.
        KEY_TYPE offset = partial_sums[tid];
        #pragma omp for schedule(static)
        for (INT_TYPE i = 0; i < N; i++) {
            offset += key_buff_ptr[start_key + i].load(std::memory_order_relaxed);
            cumulative[start_key + i] = offset;
        }
    }

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

/*  Generate random number sequence and subsequent keys on all procs */
    create_seq( find_my_seed( my_rank,
                              comm_size,
                              4*(long)TOTAL_KEYS*MIN_PROCS,
                              314159265.00,      /* Random number gen seed */
                              1220703125.00 ),   /* Random number gen mult */
                1220703125.00 );                 /* Random number gen mult */

/*  Initialize LCI active message properties */
    send_counter = lci::alloc_counter();
    send_bucket_handler = lci::alloc_handler_x(am_handler).zero_copy_am(use_upacket == 1)();
    send_bucket_rcomp = lci::register_rcomp(send_bucket_handler);
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

    lci::free_comp(&send_bucket_handler);
    // Free pre-allocated buffer
    if (preallocated_buffer != nullptr) {
        free(preallocated_buffer);
        preallocated_buffer = nullptr;
    }
    free_devices();
    lci::g_runtime_fina();

    return 0;
         /**************************/
}        /*  E N D  P R O G R A M  */
         /**************************/
