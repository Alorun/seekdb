/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;

  // Reusable GPU search buffers.
  float *query_buf;
  unsigned *ids_buf;
  float *dist_buf;
  size_t query_capacity;
  size_t result_capacity;
  size_t dist_capacity;

  // Reused across searches.
  cuvsCagraSearchParams_t search_params;
} sk_cuvs_index;

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 64;
  h->ip->intermediate_graph_degree = 128;
  h->ip->nn_descent_niter = 3;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}


static int ensure_device_buffer(
    cuvsResources_t res,
    void **ptr,
    size_t *capacity,
    size_t required,
    size_t elem_size)
{
  if (required <= *capacity) return 0;
  if (required > SIZE_MAX / elem_size) return -1;

  size_t new_capacity = *capacity ? *capacity : 64;

  while (new_capacity < required) {
    if (new_capacity > SIZE_MAX / 2) {
      new_capacity = required;
      break;
    }
    new_capacity *= 2;
  }

  void *new_ptr = NULL;

  if (cuvsRMMAlloc(res, &new_ptr,
                   new_capacity * elem_size) != CUVS_SUCCESS) {
    return -1;
  }

  if (*ptr != NULL) {
    cuvsRMMFree(res, *ptr, *capacity * elem_size);
  }

  *ptr = new_ptr;
  *capacity = new_capacity;

  return 0;
}


int seekdb_cuvs_search(void* handle, const float* query, long nq,
                       long topk, unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;

  if (!h || !query || !out_ids ||
      nq <= 0 || topk <= 0 || h->dim <= 0) {
    return 1;
  }

  if ((size_t)nq > SIZE_MAX / (size_t)h->dim ||
      (size_t)nq > SIZE_MAX / (size_t)topk) {
    return 2;
  }

  size_t q_count = (size_t)nq * (size_t)h->dim;
  size_t result_count = (size_t)nq * (size_t)topk;

  int rc = 0;

  // Reuse query buffer.
  if (ensure_device_buffer(h->res, (void**)&h->query_buf,
                           &h->query_capacity,
                           q_count, sizeof(float)) != 0) {
    rc = 10;
    goto done;
  }

  // Reuse neighbor ID buffer.
  if (ensure_device_buffer(h->res, (void**)&h->ids_buf,
                           &h->result_capacity,
                           result_count, sizeof(unsigned)) != 0) {
    rc = 11;
    goto done;
  }

  // Reuse distance buffer.
  if (ensure_device_buffer(h->res, (void**)&h->dist_buf,
                           &h->dist_capacity,
                           result_count, sizeof(float)) != 0) {
    rc = 12;
    goto done;
  }

  // Create search parameters only once per index.
  if (h->search_params == NULL) {
    if (cuvsCagraSearchParamsCreate(&h->search_params) != CUVS_SUCCESS) {
      rc = 14;
      goto done;
    }

    h->search_params->itopk_size = 256;
    h->search_params->search_width = 4;
  }

  if (cudaMemcpy(h->query_buf, query,
                 q_count * sizeof(float),
                 cudaMemcpyDefault) != cudaSuccess) {
    rc = 13;
    goto done;
  }

  {
    DLManagedTensor qt, nt, dt;
    long qs[2] = {nq, h->dim};
    long nsh[2] = {nq, topk};
    long dsh[2] = {nq, topk};

    qt.dl_tensor.data = h->query_buf;
    qt.dl_tensor.device.device_type = kDLCUDA;
    qt.dl_tensor.device.device_id = 0;
    qt.dl_tensor.ndim = 2;
    qt.dl_tensor.dtype.code = kDLFloat;
    qt.dl_tensor.dtype.bits = 32;
    qt.dl_tensor.dtype.lanes = 1;
    qt.dl_tensor.shape = qs;
    qt.dl_tensor.strides = 0;
    qt.dl_tensor.byte_offset = 0;

    nt = qt;
    nt.dl_tensor.data = h->ids_buf;
    nt.dl_tensor.dtype.code = kDLUInt;
    nt.dl_tensor.shape = nsh;

    dt = qt;
    dt.dl_tensor.data = h->dist_buf;
    dt.dl_tensor.dtype.code = kDLFloat;
    dt.dl_tensor.shape = dsh;

    cuvsFilter f;
    f.type = NO_FILTER;
    f.addr = (uintptr_t)0;

    if (cuvsCagraSearch(h->res, h->search_params,
                        h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) {
      rc = 15;
      goto done;
    }
  }

  if (cudaMemcpy(out_ids, h->ids_buf,
                 result_count * sizeof(unsigned),
                 cudaMemcpyDefault) != cudaSuccess) {
    rc = 16;
    goto done;
  }

  if (out_dist != NULL &&
      cudaMemcpy(out_dist, h->dist_buf,
                 result_count * sizeof(float),
                 cudaMemcpyDefault) != cudaSuccess) {
    rc = 17;
    goto done;
  }

done:
  if (rc != 0) {
    fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n",
            rc, cuvsGetLastErrorText());
  }

  return rc;
}


void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;

  if (h->search_params != NULL) {
    cuvsCagraSearchParamsDestroy(h->search_params);
  }

  if (h->dist_buf != NULL) {
    cuvsRMMFree(h->res, h->dist_buf,
                h->dist_capacity * sizeof(float));
  }

  if (h->ids_buf != NULL) {
    cuvsRMMFree(h->res, h->ids_buf,
                h->result_capacity * sizeof(unsigned));
  }

  if (h->query_buf != NULL) {
    cuvsRMMFree(h->res, h->query_buf,
                h->query_capacity * sizeof(float));
  }

  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
