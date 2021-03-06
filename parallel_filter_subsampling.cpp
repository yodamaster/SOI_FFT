#include <assert.h>
#include <float.h>

#include <boost/preprocessor/repetition/repeat.hpp>

#include <omp.h>

#include "soi.h"

/*
%..This is the step for filter and subsample.
%..In the actual cluster version, each processor runs this and produce
%..its portion of the distributed gamma_tilde.
%..In the matlab serial simulation, this routine produces gamma_tilde_dt
%..for each of the processor. But it still imitate the actual computational
%..flow of a cluster version.
%..The filter and subsample step is nothing more than a matrix multiplied
%..to the vector alpha. In the global view, it is looks like
%
%   <-   B crosses   ->
% [ x x x x x x x x x x  <-              ....   ]   [ alpha_0:S-1   ]          
% [ x x x x x x x x x x   |                     ]   [ alpha_S:2S-1  ]
% [ x x x x x x x x x x   | n_mu                ]   [ alpha_2S:3S-1 ]
% [ x x x x x x x x x x   |                     ]   [               ]
% [ x x x x x x x x x x  <-                     ]   [   ...         ]
% [ <-----> x x x x x x x x x x                 ]   [               ]
% [  d_mu   x x x x x x x x x x                 ]   [               ]
% [         x x x x x x x x x x                 ]   [               ]
% [         ...........                         ]   [               ]
% [                                             ]   [               ]
%
%..The matrix is mu x N -by- N and thus in the end gamma_tilde is mu*N
%..long (due to oversampling). Each "x" in the matrix represent a S-by-S
%..block. In parallel, each processor aims at computing a section of rows
%..of this matrix-vector product. For example, processor 0 wants to compute
%..the first   mu x N / P rows, which is   mu x N / (PxS)  of S-block-rows 
%..Because of the alignment, the very first matrix rows needs the very
%..elements of the alpha that it has. But towards the end, Processor 0 will
%..need some alpha's from the next processor.
%..We call those "ghost" values. So each processor try to get ghost value
%..from the "next" processor. In MPI, this means receiving data from the
%..next processor as well as sending some of its top "alpha" elements to
%..previous processor.
%..One more remark, the matrix block represented by a block "x" has an
%..important structure that we need to exploit. It is of the form
%..  DFT_S x diag(  segment of S-length of the w filter )
%..This reduces the computation cost from S^2  to  S log(S).
*/

extern double get_cpu_freq();

template<int N_MU = 5, int D_MU = 4>
void parallel_filter_subsampling(soi_desc_t * d, cfft_complex_t * alpha_dt)
{
  cfft_complex_t *gamma_tilde_dt = d->gamma_tilde;
  cfft_complex_t *w = d->w;
  cfft_size_t B = d->B;
	MPI_Request request_send, request_receive;

	MPI_Comm comm = d->comm;
	cfft_size_t P = d->P;
	cfft_size_t rank = d->rank;
	cfft_size_t PID_left = (P+rank-1)%P;
	cfft_size_t PID_right = (rank+1)%P;

  cfft_size_t S = d->k*d->P; // total number of segments
	cfft_size_t d_mu = d->d_mu;
	cfft_size_t n_mu = d->n_mu;
  cfft_size_t M = d->N/S; // length of one segment, before oversampling
  cfft_size_t M_hat = d->n_mu*M/d->d_mu; // length of one segment, after oversampling

/*
%..Let's begin. First carry out the computation with data that is already
%..in the processor.
%..We compute n_mu block rows as a unit because they all start at the same
%..column number (that is, they use the same alpha data). After n_mu block,
%..the rows right shift by d_mu blocks. 
%..The global alpha data has  M=(N/S)  blocks of S-block. So each processor
%..has M/P blocks of alpha. Also, because each row of the matrix is B
%..blocks, we can compute  FLOOR( ((M/P) - B)/d_mu  )  S-blocks of
%..gamma_tilde without need some alpha that is not in the processor.
*/

/*
%...first compute the gamma_tilde that requires only alpha that the
%...processor has
*/
	cfft_size_t K_0 = floor(  ((M/P)-B) / d_mu );
  if (M/P < B) {
    if (0 == rank) {
      fprintf(stderr, "input size too small\n");
    }
    exit(0);
  }
  if (0 == rank)
    printf(
      "k = %ld, S = %ld, M = %ld, M_hat = %ld, K_0 = %ld\n",
      d->k, S, M, M_hat, K_0);

MPI_TIMED_SECTION_BEGIN();
	cfft_size_t b_cnt = M/P - K_0*d_mu;
  memcpy(d->alpha_ghost, alpha_dt + K_0*d_mu*S, b_cnt*S*sizeof(cfft_complex_t));
	cfft_size_t n_elements = (B-d_mu)*S;
	cfft_size_t addr_start = b_cnt*S;
	CFFT_ASSERT_MPI( MPI_Irecv(d->alpha_ghost + addr_start, n_elements*2, 
							   MPI_TYPE, PID_right, 0, d->comm, &request_receive) );
	CFFT_ASSERT_MPI( MPI_Isend(alpha_dt, n_elements*2,
                 MPI_TYPE, PID_left, 0, d->comm, &request_send) );
MPI_TIMED_SECTION_END_WO_NEWLINE(d->comm, "\ttime_fss_ghost");

  unsigned long long conv_clks = 0, fft_clks = 0, transpose_clks = 0;
  int nthreads = omp_get_max_threads();

#ifdef SOI_MEASURE_LOAD_IMBALANCE
  for (int i = 0; i < omp_get_max_threads(); i++)
    load_imbalance_times[i] = 0;
#endif

#ifdef __AVX512F__
  const int REG_BLOCK_SIZE = 30; // use at most 30 SIMD registers out of 32
#else
  const int REG_BLOCK_SIZE = 14; // use at most 14 SIMD registers out of 16
#endif

  const int THETA_UNROLL_FACTOR = N_MU <= REG_BLOCK_SIZE ? N_MU : REG_BLOCK_SIZE;
  const int J_UNROLL_FACTOR = REG_BLOCK_SIZE/THETA_UNROLL_FACTOR < 1 ? 1 : REG_BLOCK_SIZE/THETA_UNROLL_FACTOR;

  int num_thread_groups = MIN(S/(CACHE_LINE_LEN/2), 8);
  if (0 == rank && nthreads < num_thread_groups) {
    fprintf(stderr, "OMP_NUM_THREADS should be greater than equal to %d. Consider increasing OMP_NUM_THREADS or decreasing k\n", num_thread_groups);
    exit(-1);
  }

#pragma omp parallel
  {
  size_t input_buffer_len = 128; // B + (J_UNROLL_FACTOR - 1)*d_mu
  __declspec(aligned(64)) SIMDFPTYPE input_buffer[input_buffer_len*2];
  size_t input_buffer_ptr = 0;

  int threadid = omp_get_thread_num();

  // Assume 8 cores.
  // Assume SMT is used when OMP_NUM_THREADS=16, and
  // SMT is not used when OMP_NUM_THREADS=8.
  //int core_id = threadid%8;
  //int smt_id = threadid/8;
  //assert(16 == nthreads || 8 == nthreads);
  int threadid_trans = threadid; // (16 == nthreads ? 2 : 1)*core_id + smt_id;

  // S is blocked by 8 thread groups
  size_t thread_group = threadid_trans/(nthreads/num_thread_groups);
  assert(nthreads%num_thread_groups == 0);
  size_t i_per_thread_group = S/num_thread_groups; // assume num_thread_groups divides S
  assert(S%num_thread_groups == 0);
  size_t i_begin = MIN(thread_group*i_per_thread_group, S);
  size_t i_end = MIN(i_begin + i_per_thread_group, S);

  size_t group_local_thread_id = threadid_trans%(nthreads/num_thread_groups);
  size_t end = K_0/J_UNROLL_FACTOR*J_UNROLL_FACTOR;
  size_t j_per_thread = (end + nthreads - 1)/(nthreads/num_thread_groups);
  j_per_thread = (j_per_thread + J_UNROLL_FACTOR - 1)/J_UNROLL_FACTOR*J_UNROLL_FACTOR;
  size_t j_begin = MIN(j_per_thread*group_local_thread_id, end);
  size_t j_end = MIN(j_begin + j_per_thread, end);

  unsigned long long t1 = __rdtsc();

  for (cfft_size_t i = i_begin; i < i_end; i += CACHE_LINE_LEN/2) {
    input_buffer_ptr = 0;
    for (cfft_size_t k = 0; k < B - d_mu; k++) {
      input_buffer[2*k] = _MM_LOAD((VAL_TYPE *)(alpha_dt + (j_begin*d_mu + k)*S + i));
      input_buffer[2*k + 1] = _MM_LOAD((VAL_TYPE *)(alpha_dt + (j_begin*d_mu + k)*S + i) + SIMD_WIDTH);
    }

    for (cfft_size_t j0 = j_begin; j0 < j_end; j0 += J_UNROLL_FACTOR) {

#pragma unroll(D_MU*J_UNROLL_FACTOR)
      for (int k = 0; k < D_MU*J_UNROLL_FACTOR; ++k) {
        input_buffer[(input_buffer_ptr + B - d_mu + k)%input_buffer_len*2] =
          _MM_LOAD((VAL_TYPE *)(alpha_dt + (j0*d_mu + B - d_mu + k)*S + i));
        input_buffer[(input_buffer_ptr + B - d_mu + k)%input_buffer_len*2 + 1] =
          _MM_LOAD((VAL_TYPE *)(alpha_dt + (j0*d_mu + B - d_mu + k)*S + i) + SIMD_WIDTH);
        //_MM_PREFETCH1(alpha_dt + (j0*d_mu + B + k)*S + i);
      }

      cfft_complex_t *v_tmp = gamma_tilde_dt + S*j0*N_MU + i;

      for (cfft_size_t theta_0 = 0; theta_0 < N_MU; theta_0 += THETA_UNROLL_FACTOR) {

        SIMDFPTYPE *in = d->w_dup + i*B*N_MU;

        SIMDFPTYPE xl[THETA_UNROLL_FACTOR][2], xh[THETA_UNROLL_FACTOR][2];

        cfft_size_t kkk = 0;
#pragma unroll(THETA_UNROLL_FACTOR)
        for (int theta = 0; theta < THETA_UNROLL_FACTOR; ++theta) {
          xl[theta][0] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 0*2);
          xh[theta][0] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 0*2 + 1);

          xl[theta][1] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 1*2);
          xh[theta][1] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 1*2 + 1);
        }

        SIMDFPTYPE temp[J_UNROLL_FACTOR][THETA_UNROLL_FACTOR][2];
        SIMDFPTYPE ytemp[J_UNROLL_FACTOR][2];

#pragma unroll(J_UNROLL_FACTOR)
        for (int j = 0; j < J_UNROLL_FACTOR; ++j) {
          ytemp[j][0] = _MM_LOAD(
            input_buffer + (input_buffer_ptr + j*d_mu + kkk)%input_buffer_len*2 + 0);
          ytemp[j][1] = _MM_LOAD(
            input_buffer + (input_buffer_ptr + j*d_mu + kkk)%input_buffer_len*2 + 1);
        }

#pragma unroll(J_UNROLL_FACTOR)
        for (int j = 0; j < J_UNROLL_FACTOR; ++j) {
#pragma unroll(THETA_UNROLL_FACTOR)
          for (int theta = 0; theta < THETA_UNROLL_FACTOR; ++theta) {
            temp[j][theta][0] = _MM_FMADDSUB(
              xl[theta][0], ytemp[j][0],
              _MM_SWAP_REAL_IMAG(_MM_MUL(xh[theta][0], ytemp[j][0])));
            temp[j][theta][1] = _MM_FMADDSUB(
              xl[theta][1], ytemp[j][1],
              _MM_SWAP_REAL_IMAG(_MM_MUL(xh[theta][1], ytemp[j][1])));
          }
        }

        for (kkk = 1; kkk < B; kkk++) {
#pragma unroll(THETA_UNROLL_FACTOR)
          for (int theta = 0; theta < THETA_UNROLL_FACTOR; ++theta) {
            xl[theta][0] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 0*2);
            xh[theta][0] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 0*2 + 1);

            xl[theta][1] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 1*2);
            xh[theta][1] = _MM_LOAD(in + (kkk*N_MU + theta_0 + theta)*(CACHE_LINE_LEN/2) + 1*2 + 1);
          }

#pragma unroll(J_UNROLL_FACTOR)
          for (int j = 0; j < J_UNROLL_FACTOR; ++j) {
            ytemp[j][0] = _MM_LOAD(
              input_buffer + (input_buffer_ptr + j*d_mu + kkk)%input_buffer_len*2 + 0);
            ytemp[j][1] = _MM_LOAD(
              input_buffer + (input_buffer_ptr + j*d_mu + kkk)%input_buffer_len*2 + 1);
          }

#pragma unroll(J_UNROLL_FACTOR)
          for (int j = 0; j < J_UNROLL_FACTOR; ++j) {
#pragma unroll(THETA_UNROLL_FACTOR)
            for (int theta = 0; theta < THETA_UNROLL_FACTOR; ++theta) {
              temp[j][theta][0] = _MM_ADD(
                temp[j][theta][0],
                _MM_FMADDSUB(
                  xl[theta][0], ytemp[j][0],
                  _MM_SWAP_REAL_IMAG(_MM_MUL(xh[theta][0], ytemp[j][0]))));
              temp[j][theta][1] = _MM_ADD(
                temp[j][theta][1],
                _MM_FMADDSUB(
                  xl[theta][1], ytemp[j][1],
                  _MM_SWAP_REAL_IMAG(_MM_MUL(xh[theta][1], ytemp[j][1]))));
            }
          }
        }

#pragma unroll(J_UNROLL_FACTOR)
        for (int j = 0; j < J_UNROLL_FACTOR; ++j) {
#pragma unroll(THETA_UNROLL_FACTOR)
          for (int theta = 0; theta < THETA_UNROLL_FACTOR; ++theta) {
            _MM_STREAM(
              (VAL_TYPE *)(v_tmp + S*(j*N_MU + theta_0 + theta)),
              temp[j][theta][0]);
            _MM_STREAM(
              (VAL_TYPE *)(v_tmp + S*(j*N_MU + theta_0 + theta)) + SIMD_WIDTH,
              temp[j][theta][1]);
          }
        }

        /*if (0 == rank && 0 == threadid) {
          for (int t = 0; t < THETA_UNROLL_FACTOR; ++t) {
            cfft_complex_t c = v_tmp[S*(theta + t)];
            printf("(%g %g) ", __real__(c), __imag__(c));
          }
          printf("\n");
        }*/
      } // theta

      input_buffer_ptr = (input_buffer_ptr + d_mu*J_UNROLL_FACTOR)%input_buffer_len;
    } // JJ
  } // i

#pragma omp barrier

  if (0 == threadid) conv_clks += __rdtsc() - t1;

  j_per_thread = (end + nthreads - 1)/nthreads;
  j_begin = MIN(j_per_thread*threadid_trans, end);
  j_end = MIN(j_begin + j_per_thread, end);

  for (cfft_size_t j = j_begin; j < j_end; j++) {
    cfft_complex_t *v_tmp = gamma_tilde_dt + S*j*n_mu;

    unsigned long long t2 = __rdtsc();
    for (int theta = 0; theta < N_MU; theta++) {
      /*if (0 == rank && 0 == threadid) {
        for (int k = 0; k < S; ++k) {
          printf("(%g %g) ", __real__(v_tmp[S*theta + k]), __real__(v_tmp[S*theta + k]));
        }
        printf("\n");
      }*/
      DftiComputeForward(d->desc_dft_s, v_tmp + S*theta);
      for (size_t i = 0; i < S; i += CACHE_LINE_LEN)
        _MM_PREFETCH1(v_tmp + S*(theta + n_mu) + i);
    }

    unsigned long long t3 = __rdtsc();
    cfft_size_t l = M_hat/d->P;

#ifdef __AVX__
    if (8 == N_MU) {
      for (int jj = j*n_mu ; jj < (j + 1)*n_mu/SIMD_WIDTH*SIMD_WIDTH; jj += 2*SIMD_WIDTH) {
        cfft_size_t s = 0;
        for (cfft_size_t s = 0; s < S; s += SIMD_WIDTH) {
#if PRECISION == 1
          // TODO!
#else
          cfft_complex_t *in = v_tmp + S*(jj - j*n_mu) + s;

          SIMDFPTYPE a11 = _MM_LOAD(in);
          SIMDFPTYPE a21 = _MM_LOAD(in + 2);
          SIMDFPTYPE a12 = _MM_LOAD(in + 1*S);
          SIMDFPTYPE a22 = _MM_LOAD(in + 1*S + 2);
          SIMDFPTYPE a31 = _MM_LOAD(in + 2*S);
          SIMDFPTYPE a41 = _MM_LOAD(in + 2*S + 2);
          SIMDFPTYPE a32 = _MM_LOAD(in + 3*S);
          SIMDFPTYPE a42 = _MM_LOAD(in + 3*S + 2);

          SIMDFPTYPE a51 = _MM_LOAD(in + 4*S);
          SIMDFPTYPE a61 = _MM_LOAD(in + 4*S + 2);
          SIMDFPTYPE a52 = _MM_LOAD(in + 5*S);
          SIMDFPTYPE a62 = _MM_LOAD(in + 5*S + 2);
          SIMDFPTYPE a71 = _MM_LOAD(in + 6*S);
          SIMDFPTYPE a81 = _MM_LOAD(in + 6*S + 2);
          SIMDFPTYPE a72 = _MM_LOAD(in + 7*S);
          SIMDFPTYPE a82 = _MM_LOAD(in + 7*S + 2);

          SIMDFPTYPE b11 = _mm256_insertf128_pd(a11, _mm256_castpd256_pd128(a12), 1);
          SIMDFPTYPE b12 = _mm256_permute2f128_pd(a11, a12, 0x31);
          SIMDFPTYPE b21 = _mm256_insertf128_pd(a21, _mm256_castpd256_pd128(a22), 1);
          SIMDFPTYPE b22 = _mm256_permute2f128_pd(a21, a22, 0x31);
          SIMDFPTYPE b31 = _mm256_insertf128_pd(a31, _mm256_castpd256_pd128(a32), 1);
          SIMDFPTYPE b32 = _mm256_permute2f128_pd(a31, a32, 0x31);
          SIMDFPTYPE b41 = _mm256_insertf128_pd(a41, _mm256_castpd256_pd128(a42), 1);
          SIMDFPTYPE b42 = _mm256_permute2f128_pd(a41, a42, 0x31);

          SIMDFPTYPE b51 = _mm256_insertf128_pd(a51, _mm256_castpd256_pd128(a52), 1);
          SIMDFPTYPE b52 = _mm256_permute2f128_pd(a51, a52, 0x31);
          SIMDFPTYPE b61 = _mm256_insertf128_pd(a61, _mm256_castpd256_pd128(a62), 1);
          SIMDFPTYPE b62 = _mm256_permute2f128_pd(a61, a62, 0x31);
          SIMDFPTYPE b71 = _mm256_insertf128_pd(a71, _mm256_castpd256_pd128(a72), 1);
          SIMDFPTYPE b72 = _mm256_permute2f128_pd(a71, a72, 0x31);
          SIMDFPTYPE b81 = _mm256_insertf128_pd(a81, _mm256_castpd256_pd128(a82), 1);
          SIMDFPTYPE b82 = _mm256_permute2f128_pd(a81, a82, 0x31);

          cfft_complex_t *out = d->alpha_tilde + s*l + jj;

          _MM_STREAM(out, b11);
          _MM_STREAM(out + 2, b31);
          _MM_STREAM(out + 4, b51);
          _MM_STREAM(out + 6, b71);
          _MM_STREAM(out + l, b12);
          _MM_STREAM(out + l + 2, b32);
          _MM_STREAM(out + l + 4, b52);
          _MM_STREAM(out + l + 6, b72);
          _MM_STREAM(out + 2*l, b21);
          _MM_STREAM(out + 2*l + 2, b41);
          _MM_STREAM(out + 2*l + 4, b61);
          _MM_STREAM(out + 2*l + 6, b81);
          _MM_STREAM(out + 3*l, b22);
          _MM_STREAM(out + 3*l + 2, b42);
          _MM_STREAM(out + 3*l + 4, b62);
          _MM_STREAM(out + 3*l + 6, b82);
#endif // PRECISION
        }
      }
    }
    else
#endif // __AVX__
    { // N_MU == 8
      for (cfft_size_t theta = 0; theta < n_mu; ++theta) {
        for (int s = 0; s < S; s++) {
          cfft_size_t l = M_hat/d->P;

          d->alpha_tilde[s*l + j*n_mu + theta] =
            gamma_tilde_dt[S*(j*n_mu + theta) + s];
        }
      }
    }

    if (0 == threadid) {
      transpose_clks += __rdtsc() - t3;
      fft_clks += t3 - t2;
    }
  } // for (cfft_size_t j=0; j<K_0; j++)

#pragma omp for
  for (cfft_size_t j = K_0/J_UNROLL_FACTOR*J_UNROLL_FACTOR; j < K_0; j++) {
	  for (cfft_size_t theta = 0; theta < n_mu; theta++) {
      unsigned long long t1 = __rdtsc();
			cfft_complex_t *v_tmp = gamma_tilde_dt + S*(j*n_mu + theta);
      for (cfft_size_t i = 0; i < S; i += CACHE_LINE_LEN/2) {
        for (cfft_size_t ii = 0; ii < 2; ii++) {
          SIMDFPTYPE *in = d->w_dup + i*B*n_mu + theta*(CACHE_LINE_LEN/2) + ii*2;
          //VAL_TYPE *in_end = d->w_dup + ((i + SIMD_WIDTH/2)*B*n_mu + theta*(CACHE_LINE_LEN/2))*SIMD_WIDTH;

          SIMDFPTYPE xl = _MM_LOAD(in);
          SIMDFPTYPE xh = _MM_LOAD(in + 1);
          SIMDFPTYPE ytemp = _MM_LOAD((VAL_TYPE *)(alpha_dt + j*d_mu*S + i) + ii*SIMD_WIDTH);
          SIMDFPTYPE temp = _MM_FMADDSUB(xl, ytemp, _MM_SWAP_REAL_IMAG(_MM_MUL(xh, ytemp)));

          in += n_mu*(CACHE_LINE_LEN/2);

          for (cfft_size_t kkk = 1; kkk < B; kkk++, in += n_mu*(CACHE_LINE_LEN/2)) {
            xl = _MM_LOAD(in);
            xh = _MM_LOAD(in + 1);
            ytemp = _MM_LOAD((VAL_TYPE *)(alpha_dt + (j*d_mu + kkk)*S + i) + ii*SIMD_WIDTH);
            temp = _MM_ADD(temp, _MM_FMADDSUB(xl, ytemp, _MM_SWAP_REAL_IMAG(_MM_MUL(xh, ytemp))));
          }
          _MM_STORE((VAL_TYPE *)(v_tmp + i) + ii*SIMD_WIDTH, temp);
        }
      }

      unsigned long long t2 = __rdtsc();

			DftiComputeForward(d->desc_dft_s, v_tmp);

      for (int s = 0; s < S; s++) {
        cfft_size_t l = M_hat/d->P;

        d->alpha_tilde[s*l + j*n_mu + theta] =
          gamma_tilde_dt[S*(j*n_mu + theta) + s];
      }

      if (0 == threadid) {
        conv_clks += t2 - t1;
        fft_clks += __rdtsc() - t2;
      }
		} // for (cfft_size_t theta=0; theta<n_mu; theta++)
  } // for (cfft_size_t j = K_0/J_UNROLL_FACTOR*J_UNROLL_FACTOR; j += J_UNROLL_FACTOR)

#ifdef SOI_MEASURE_LOAD_IMBALANCE
  unsigned long long t = __rdtsc();
#pragma omp barrier
  load_imbalance_times[threadid] += __rdtsc() - t;
#endif

  }
  if (0 == rank) {
    printf("\ttime_fss_conv\t%f", conv_clks/get_cpu_freq());
    printf("\ttime_fss_fft\t%f", fft_clks/get_cpu_freq());
    printf("\ttime_fss_trans\t%f", transpose_clks/get_cpu_freq());
  }

/*
%...Now compute the rest. These will need some of the bottom part of the
%...alpha that the current processor has, but also some from the "right"
%...neighbor, i.e. processor  (rank+1)mod P
%...To make the code simplier, we will pack all these needed alpha into
%...the data array  alpha_ghost_dt. We start by packing the alpha the
%...processor has into alpha_ghost_dt, followed by "receiving" the
%...remaining needed ones from its neigbor processor.
*/

/*
%...now we will "receive" from the next neighbor several block of alpha.
%...The exact number needed is  B-d_mu, that is, the total number of 
%...elements is  (B-d_mu)*S. Thus, in reality, each processor will
%...receive  (B-d_mu)*S complex elements from its right neighbor, as well
%...as send (B-d_mu)*S of its own top elements to its left neighbor.
%...Here in matlab, we will only "receive" by copying the required data.

%...alpha_ghost already has b_cnt-1 block of S elements filled up.
%...so starting address is  b_cnt*S, ending address is b_cnt*S+(B-d_mu)*S-1
*/
MPI_TIMED_SECTION_BEGIN();
	CFFT_ASSERT_MPI( MPI_Wait(&request_receive, MPI_STATUS_IGNORE) );
MPI_TIMED_SECTION_END_WO_NEWLINE(d->comm, "\ttime_fss_mpi");

/*
%...now finish the remaining computation of gamma_tilde
%...total length of gamma_tilde is mu * N, thus each processor
%...has mu*N/P which is mu*M/P blocks of S length. We have
%...computed K_0*n_mu number of blocks (the loop is double nested)
%...the inner loop goes from theta=0:n_mu-1.
%...Thus total number of n_mu*S block of gamma_tilde to be computed 
%...in this processor is    mu*M/(P*n_mu) - K_0
*/
MPI_TIMED_SECTION_BEGIN();
#pragma omp parallel for
  for (cfft_size_t j=0; j<(M_hat/(P*n_mu))-K_0; j++)
    for (cfft_size_t theta=0; theta<n_mu; theta++) {
      cfft_complex_t *v_tmp = gamma_tilde_dt + (K_0*n_mu + j*n_mu + theta)*S;
      for (cfft_size_t i = 0; i < S; i += CACHE_LINE_LEN/2) {
        for (cfft_size_t ii = 0; ii < 2; ii++) {
          SIMDFPTYPE xl = _MM_LOAD(d->w_dup + i*B*n_mu + theta*(CACHE_LINE_LEN/2) + ii*2);
          SIMDFPTYPE xh = _MM_LOAD(d->w_dup + i*B*n_mu + theta*(CACHE_LINE_LEN/2) + ii*2 + 1);
          SIMDFPTYPE ytemp = _MM_LOAD((VAL_TYPE *)(d->alpha_ghost + j*d_mu*S + i) + ii*SIMD_WIDTH);
          SIMDFPTYPE temp = _MM_FMADDSUB(xl, ytemp, _MM_SWAP_REAL_IMAG(_MM_MUL(xh, ytemp)));

          for (cfft_size_t kkk=1; kkk<B; kkk++) {
            xl = _MM_LOAD(d->w_dup + i*B*n_mu + (kkk*n_mu + theta)*(CACHE_LINE_LEN/2) + ii*2);
            xh = _MM_LOAD(d->w_dup + i*B*n_mu + (kkk*n_mu + theta)*(CACHE_LINE_LEN/2) + ii*2 + 1);
            ytemp = _MM_LOAD((VAL_TYPE *)(d->alpha_ghost + (j*d_mu + kkk)*S + i) + ii*SIMD_WIDTH);
            temp = _MM_ADD(temp, _MM_FMADDSUB(xl, ytemp, _MM_SWAP_REAL_IMAG(_MM_MUL(xh, ytemp))));
          }
          _MM_STORE((VAL_TYPE *)(v_tmp + i) + ii*SIMD_WIDTH, temp);
        }
      }

      DftiComputeForward(d->desc_dft_s, v_tmp);

      for (int s = 0; s < S; s++) {
        cfft_size_t l = M_hat/d->P;

        d->alpha_tilde[s*l + (K_0 + j)*n_mu + theta] = v_tmp[s];
      }
    }
	CFFT_ASSERT_MPI( MPI_Wait(&request_send, MPI_STATUS_IGNORE) );
MPI_TIMED_SECTION_END(d->comm, "\ttime_fss_last");
}

extern "C"
{
extern void parallel_filter_subsampling_n_mu_8(soi_desc_t * d, cfft_complex_t * alpha_dt);
}

__declspec(noinline)
void parallel_filter_subsampling(soi_desc_t * d, cfft_complex_t * alpha_dt)
{
  if (5 == d->n_mu && 4 == d->d_mu) {
    parallel_filter_subsampling<5, 4>(d, alpha_dt);
  }
  else if (8 == d->n_mu && 7 == d->d_mu) {
    parallel_filter_subsampling<8, 7>(d, alpha_dt);
  }
  else {
    if (0 == d->rank) {
      fprintf(stderr, "Unsupported n_mu and d_mu. Try n_mu=5 && d_mu=4 or n_mu=8 && d_mu=7\n");
    }
    exit(-1);
    assert(0);
  }
}
