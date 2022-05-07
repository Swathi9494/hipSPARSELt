/* ************************************************************************
 * Copyright (c) 2021-2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ************************************************************************ */

#include "definitions.h"
#include "handle.h"
#include "rocsparselt.h"
//#include "utility.hpp"

#include <hip/hip_runtime_api.h>

template <typename Ti, int SG0I, int SG1J, int TT0I, int TT1J>
__global__ void prune_check_kernel(const Ti* in,
                                   int*      out,
                                   int64_t   m,
                                   int64_t   n,
                                   int64_t   stride1,
                                   int64_t   stride2,
                                   int       num_batches,
                                   int64_t   batch_stride,
                                   int64_t   sizes)
{
    constexpr unsigned int MT0I = SG0I * TT0I;
    constexpr unsigned int MT1J = SG1J * TT1J;

    unsigned int serial = hc_get_workitem_id(0);
    unsigned int sg0I   = serial % SG0I;
    unsigned int sg1J   = serial / SG0I;

    int64_t stride = sg0I * stride1 + sg1J * TT1J * stride2;

    unsigned int wg0I    = hc_get_group_id(0);
    unsigned int wg1J    = hc_get_group_id(1);
    unsigned int batchId = hc_get_group_id(2);

    if((MT1J * wg1J + sg1J * TT1J) >= n || (MT0I * wg0I + sg0I * TT0I) >= m)
        return;

    int64_t wg_stride = MT1J * wg1J * stride2 + MT0I * wg0I * stride1;
    int64_t b_stride  = batchId * batch_stride;

    int64_t globalReadOffset = b_stride + wg_stride + stride;

    for(int i = 0; i < TT0I; i++)
    {
        for(int j = 0; j < TT1J; j += 4)
        {
            if(*out)
                return;

            int64_t offset = i * stride1 + j * stride2;
            int     nz     = 0;

#pragma unroll
            for(int k = 0; k < 4; k++)
            {
                int64_t pos = globalReadOffset + offset + k * stride2;
                if(pos < sizes)
                {
                    if(in[pos] > 0)
                    {
                        nz++;
                    }
                }
            }
            if(nz > 2)
            {
                *out = 1;
                return;
            }
        }
    }
}

template <typename Ti, typename Tc>
__host__ __device__ inline Tc norm1(Ti a, Ti b)
{
    Tc ac = static_cast<Tc>(a);
    Tc bc = static_cast<Tc>(b);
    return static_cast<Tc>(abs(ac) + abs(bc));
}

template <typename Ti, typename Tc, int SG0I, int SG1J, int TT0I, int TT1J, bool InPlace>
__global__ void prune_strip_kernel(const Ti* in,
                                   Ti*       out,
                                   int64_t   m,
                                   int64_t   n,
                                   int64_t   stride1,
                                   int64_t   stride2,
                                   int       num_batches,
                                   int64_t   batch_stride,
                                   int64_t   sizes)
{
    constexpr unsigned int MT0I = SG0I * TT0I;
    constexpr unsigned int MT1J = SG1J * TT1J;

    unsigned int serial = hc_get_workitem_id(0);
    unsigned int sg0I   = serial % SG0I;
    unsigned int sg1J   = serial / SG0I;
    int64_t      stride = sg0I * stride1 + sg1J * TT1J * stride2;

    unsigned int wg0I    = hc_get_group_id(0);
    unsigned int wg1J    = hc_get_group_id(1);
    unsigned int batchId = hc_get_group_id(2);

    if((MT1J * wg1J + sg1J * TT1J) >= n || (MT0I * wg0I + sg0I * TT0I) >= m)
        return;

    int64_t wg_stride = MT1J * wg1J * stride2 + MT0I * wg0I * stride1;
    int64_t b_stride  = batchId * batch_stride;

    int64_t globalReadOffset = b_stride + wg_stride + stride;

    for(int i = 0; i < TT0I; i++)
    {
        for(int j = 0; j < TT1J; j += 4)
        {
            int64_t offset = globalReadOffset + i * stride1 + j * stride2;
            Ti      values[4];
#pragma unroll
            for(int k = 0; k < 4; k++)
            {
                int64_t pos = offset + k * stride2;
                if(pos >= sizes)
                    values[k] = static_cast<Ti>(0.0f);
                else
                    values[k] = in[pos];
            }

            auto max_norm1 = static_cast<Tc>(-1.0);
            int  pos_a = 0, pos_b = 0;

#pragma unroll
            for(int a = 0; a < 4; a++)
            {
                for(int b = a + 1; b < 4; b++)
                {
                    auto norm1_v = norm1<Ti, Tc>(values[a], values[b]);
                    if(norm1_v > max_norm1)
                    {
                        pos_a     = a;
                        pos_b     = b;
                        max_norm1 = norm1_v;
                    }
                }
            }

#pragma unroll
            for(int k = 0; k < 4; k++)
            {
                int64_t pos = offset + k * stride2;
                if(k != pos_a && k != pos_b)
                    out[pos] = static_cast<Ti>(0.0f);
                else if constexpr(!InPlace)
                    out[pos] = values[k];
            }
        }
    }
}

template <typename Ti, typename Tc>
rocsparselt_status rocsparselt_smfmac_prune_template(const rocsparselt_handle handle,
                                                     int64_t                  m,
                                                     int64_t                  n,
                                                     int64_t                  stride0,
                                                     int64_t                  stride1,
                                                     int                      num_batches,
                                                     int64_t                  batch_stride,
                                                     rocsparselt_operation    op,
                                                     rocsparselt_order        order,
                                                     const Ti*                d_in,
                                                     Ti*                      d_out,
                                                     rocsparselt_prune_alg    pruneAlg,
                                                     hipStream_t              stream)
{
    constexpr int SG0I = 16;
    constexpr int SG1J = 4;
    constexpr int TT0I = 1;
    constexpr int TT1J = 4;
    constexpr int MT0I = SG0I * TT0I;
    constexpr int MT1J = SG1J * TT1J;

    int block_x = m / MT0I + (m % MT0I > 0 ? 1 : 0);
    int block_y = n / MT1J + (n % MT1J > 0 ? 1 : 0);

    if(pruneAlg == rocsparselt_prune_smfmac_strip)
    {
        void (*func)(const Ti* in,
                     Ti*       out,
                     int64_t   m,
                     int64_t   n,
                     int64_t   stride1,
                     int64_t   stride2,
                     int       num_batches,
                     int64_t   batch_stride,
                     int64_t   sizes);
        if(d_in == d_out)
            func = prune_strip_kernel<Ti, Tc, SG0I, SG1J, TT0I, TT1J, true>;
        else
            func = prune_strip_kernel<Ti, Tc, SG0I, SG1J, TT0I, TT1J, false>;
        hipLaunchKernelGGL(func, /* compute kernel*/
                           dim3(block_x, block_y, num_batches),
                           dim3(SG0I * SG1J),
                           0 /*dynamic shared*/,
                           stream,
                           d_in,
                           d_out,
                           m,
                           n,
                           stride0,
                           stride1,
                           num_batches,
                           batch_stride,
                           num_batches * batch_stride);
        return rocsparselt_status_success;
    }
    return rocsparselt_status_not_implemented;
}

template <typename Ti>
rocsparselt_status rocsparselt_smfmac_prune_check_template(const rocsparselt_handle handle,
                                                           int64_t                  m,
                                                           int64_t                  n,
                                                           int64_t                  stride0,
                                                           int64_t                  stride1,
                                                           int                      num_batches,
                                                           int64_t                  batch_stride,
                                                           rocsparselt_operation    op,
                                                           rocsparselt_order        order,
                                                           const Ti*                d_in,
                                                           int*                     d_out,
                                                           hipStream_t              stream)
{
    constexpr int SG0I = 16;
    constexpr int SG1J = 4;
    constexpr int TT0I = 1;
    constexpr int TT1J = 4;
    constexpr int MT0I = SG0I * TT0I;
    constexpr int MT1J = SG1J * TT1J;

    int block_x = m / MT0I + (m % MT0I > 0 ? 1 : 0);
    int block_y = n / MT1J + (n % MT1J > 0 ? 1 : 0);

    hipMemsetAsync(d_out, 0, sizeof(int), stream);
    hipLaunchKernelGGL((prune_check_kernel<Ti, SG0I, SG1J, TT0I, TT1J>), /* compute kernel*/
                       dim3(block_x, block_y, num_batches),
                       dim3(SG0I * SG1J),
                       0 /*dynamic shared*/,
                       stream,
                       d_in,
                       d_out,
                       m,
                       n,
                       stride0,
                       stride1,
                       num_batches,
                       batch_stride,
                       num_batches * batch_stride);
    return rocsparselt_status_success;
}

#ifdef __cplusplus
extern "C" {
#endif

rocsparselt_status rocsparselt_smfmac_prune_impl(const rocsparselt_handle handle,
                                                 int64_t                  m,
                                                 int64_t                  n,
                                                 int64_t                  stride0,
                                                 int64_t                  stride1,
                                                 int                      num_batches,
                                                 int64_t                  batch_stride,
                                                 rocsparselt_operation    op,
                                                 rocsparselt_order        order,
                                                 const void*              d_in,
                                                 void*                    d_out,
                                                 rocsparselt_datatype     in_type,
                                                 rocsparselt_compute_type compute_type,
                                                 rocsparselt_prune_alg    pruneAlg,
                                                 hipStream_t              stream)
{
#define PRUNE_PARAMS(T)                                                   \
    handle, m, n, stride0, stride1, num_batches, batch_stride, op, order, \
        reinterpret_cast<const T*>(d_in), reinterpret_cast<T*>(d_out), pruneAlg, stream

    switch(in_type)
    {
    case rocsparselt_datatype_f16_r:
        return rocsparselt_smfmac_prune_template<rocsparselt_half, float>(
            PRUNE_PARAMS(rocsparselt_half));
    case rocsparselt_datatype_bf16_r:
        return rocsparselt_smfmac_prune_template<rocsparselt_bfloat16, float>(
            PRUNE_PARAMS(rocsparselt_bfloat16));
    case rocsparselt_datatype_i8_r:
        return rocsparselt_smfmac_prune_template<int8_t, float>(PRUNE_PARAMS(int8_t));
    default:
        return rocsparselt_status_not_implemented;
    }
}

rocsparselt_status rocsparselt_smfmac_prune_check_impl(const rocsparselt_handle handle,
                                                       int64_t                  m,
                                                       int64_t                  n,
                                                       int64_t                  stride0,
                                                       int64_t                  stride1,
                                                       int                      num_batches,
                                                       int64_t                  batch_stride,
                                                       rocsparselt_operation    op,
                                                       rocsparselt_order        order,
                                                       const void*              d_in,
                                                       int*                     d_out,
                                                       rocsparselt_datatype     in_type,
                                                       hipStream_t              stream)
{
#define PRUNE_CHECK_PARAMS(T)                                             \
    handle, m, n, stride0, stride1, num_batches, batch_stride, op, order, \
        reinterpret_cast<const T*>(d_in), d_out, stream

    switch(in_type)
    {
    case rocsparselt_datatype_f16_r:
        return rocsparselt_smfmac_prune_check_template<rocsparselt_half>(
            PRUNE_CHECK_PARAMS(rocsparselt_half));
    case rocsparselt_datatype_bf16_r:
        return rocsparselt_smfmac_prune_check_template<rocsparselt_bfloat16>(
            PRUNE_CHECK_PARAMS(rocsparselt_bfloat16));
    case rocsparselt_datatype_i8_r:
        return rocsparselt_smfmac_prune_check_template<int8_t>(PRUNE_CHECK_PARAMS(int8_t));
    default:
        return rocsparselt_status_not_implemented;
    }
}

/********************************************************************************
 * \brief prunes a dense matrix according to the specified algorithm.
 *******************************************************************************/
rocsparselt_status rocsparselt_smfmac_prune(const rocsparselt_handle*       handle,
                                            const rocsparselt_matmul_descr* matmulDescr,
                                            const void*                     d_in,
                                            void*                           d_out,
                                            rocsparselt_prune_alg           pruneAlg,
                                            hipStream_t                     stream)

{
    // Check if handle is valid
    if(handle == nullptr || matmulDescr == nullptr || *handle == nullptr)
    {
        return rocsparselt_status_invalid_handle;
    }

    // Check if pointer is valid
    if(d_in == nullptr || d_out == nullptr)
    {
        return rocsparselt_status_invalid_pointer;
    }

    // Check if prune alg is valid
    if(pruneAlg != rocsparselt_prune_smfmac_strip)
    {
        return rocsparselt_status_not_implemented;
    }

    auto _matmulDescr = reinterpret_cast<const _rocsparselt_matmul_descr*>(matmulDescr);

    rocsparselt_mat_descr matrix;
    // Check if matrix A is a structured matrix
    if(_matmulDescr->matrix_A->m_type == rocsparselt_matrix_type_structured)
        matrix = _matmulDescr->matrix_A;
    else
        return rocsparselt_status_not_implemented;

    rocsparselt_operation    opA          = _matmulDescr->op_A;
    rocsparselt_compute_type compute_type = _matmulDescr->compute_type;

    int64_t              o_m   = opA == rocsparselt_operation_transpose ? matrix->n : matrix->m;
    int64_t              o_n   = opA == rocsparselt_operation_transpose ? matrix->m : matrix->n;
    int64_t              ld    = matrix->ld;
    rocsparselt_order    order = matrix->order;
    rocsparselt_datatype type  = matrix->type;

    int     num_batches  = 1;
    int64_t batch_stride = 0;
    matrix->attributes[rocsparselt_mat_num_batches].get(&num_batches);
    matrix->attributes[rocsparselt_mat_batch_stride].get(&batch_stride);

    //set the number of batches to 1 since in the broadcast case, we only care about contents in first batch.
    if(batch_stride == 0) //boardcast case.
    {
        num_batches  = 1;
        batch_stride = matrix->n * ld;
    }

    int64_t stride0 = (opA == rocsparselt_operation_transpose) ? ld : 1;
    int64_t stride1 = (opA == rocsparselt_operation_transpose) ? 1 : ld;

    return rocsparselt_smfmac_prune_impl(*handle,
                                         o_m,
                                         o_n,
                                         stride0,
                                         stride1,
                                         num_batches,
                                         batch_stride,
                                         opA,
                                         order,
                                         d_in,
                                         d_out,
                                         type,
                                         compute_type,
                                         pruneAlg,
                                         stream);
}

/********************************************************************************
 * \brief
 *******************************************************************************/
rocsparselt_status rocsparselt_smfmac_prune_check(const rocsparselt_handle*       handle,
                                                  const rocsparselt_matmul_descr* matmulDescr,
                                                  const void*                     d_in,
                                                  int*                            d_out,
                                                  hipStream_t                     stream)
{
    // Check if handle is valid
    if(handle == nullptr || matmulDescr == nullptr || *handle == nullptr)
    {
        return rocsparselt_status_invalid_handle;
    }

    // Check if pointer is valid
    if(d_in == nullptr || d_out == nullptr)
    {
        return rocsparselt_status_invalid_pointer;
    }

    auto _matmulDescr = reinterpret_cast<const _rocsparselt_matmul_descr*>(matmulDescr);

    rocsparselt_mat_descr matrix;
    // Check if matrix A is a structured matrix
    if(_matmulDescr->matrix_A->m_type == rocsparselt_matrix_type_structured)
        matrix = _matmulDescr->matrix_A;
    else
        return rocsparselt_status_not_implemented;

    rocsparselt_operation opA = _matmulDescr->op_A;

    int64_t              o_m   = opA == rocsparselt_operation_transpose ? matrix->n : matrix->m;
    int64_t              o_n   = opA == rocsparselt_operation_transpose ? matrix->m : matrix->n;
    int64_t              ld    = matrix->ld;
    rocsparselt_order    order = matrix->order;
    rocsparselt_datatype type  = matrix->type;

    int     num_batches  = 1;
    int64_t batch_stride = 0;
    matrix->attributes[rocsparselt_mat_num_batches].get(&num_batches);
    matrix->attributes[rocsparselt_mat_batch_stride].get(&batch_stride);

    //set the number of batches to 1 since in the broadcast case, we only care about contents in first batch.
    if(batch_stride == 0) //boardcast case.
    {
        num_batches  = 1;
        batch_stride = matrix->n * ld;
    }

    int64_t stride0 = (opA == rocsparselt_operation_transpose) ? ld : 1;
    int64_t stride1 = (opA == rocsparselt_operation_transpose) ? 1 : ld;

    return rocsparselt_smfmac_prune_check_impl(*handle,
                                               o_m,
                                               o_n,
                                               stride0,
                                               stride1,
                                               num_batches,
                                               batch_stride,
                                               opA,
                                               order,
                                               d_in,
                                               d_out,
                                               type,
                                               stream);
}

#ifdef __cplusplus
}
#endif
