/* ************************************************************************
 * Copyright (c) 2018-2022 Advanced Micro Devices, Inc.
 * ************************************************************************ */

#include "utility.hpp"
#include "../../library/src/include/handle.h"
#include "d_vector.hpp"
#include <chrono>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <stdlib.h>

#include <fcntl.h>

#ifdef __cpp_lib_filesystem
#include <filesystem>
#else
#include <experimental/filesystem>

namespace std
{
    namespace filesystem = experimental::filesystem;
}
#endif

/* ============================================================================================ */
// Return path of this executable
std::string rocsparselt_exepath()
{
    std::string pathstr;
    char*       path = realpath("/proc/self/exe", 0);
    if(path)
    {
        char* p = strrchr(path, '/');
        if(p)
        {
            p[1]    = 0;
            pathstr = path;
        }
        free(path);
    }
    return pathstr;
}

/* ============================================================================================ */
// Temp directory rooted random path
std::string rocsparselt_tempname()
{
    char tmp[] = "/tmp/rocsparselt-XXXXXX";
    int  fd    = mkostemp(tmp, O_CLOEXEC);
    if(fd == -1)
    {
        dprintf(STDERR_FILENO, "Cannot open temporary file: %m\n");
        exit(EXIT_FAILURE);
    }

    return std::string(tmp);
}

/* ============================================================================================ */
/*  memory allocation requirements :*/

/*! \brief Compute strided batched matrix allocation size allowing for strides smaller than full matrix */
size_t strided_batched_matrix_size(int rows, int cols, int lda, int64_t stride, int batch_count)
{
    size_t size = size_t(lda) * cols;
    if(batch_count > 1)
    {
        // for cases where batch_count strides may not exceed full matrix size use full matrix size
        // e.g. row walking a larger matrix we just use full matrix size
        size_t size_strides = (batch_count - 1) * stride;
        size += size < size_strides + (cols - 1) * size_t(lda) + rows ? size_strides : 0;
    }
    return size;
}

/* ============================================================================================ */
/*  timing:*/

/*! \brief  CPU Timer(in microsecond): synchronize with the default device and return wall time */
double get_time_us_sync_device(void)
{
    hipDeviceSynchronize();

    auto now = std::chrono::steady_clock::now();
    // now.time_since_epoch() is the duration since epoch
    // which is converted to microseconds
    auto duration
        = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return (static_cast<double>(duration));
};

/*! \brief  CPU Timer(in microsecond): synchronize with given queue/stream and return wall time */
double get_time_us_sync(hipStream_t stream)
{
    hipStreamSynchronize(stream);

    auto now = std::chrono::steady_clock::now();
    // now.time_since_epoch() is the duration since epoch
    // which is converted to microseconds
    auto duration
        = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return (static_cast<double>(duration));
};

/*! \brief  CPU Timer(in microsecond): no GPU synchronization */
double get_time_us_no_sync(void)
{
    auto now = std::chrono::steady_clock::now();
    // now.time_since_epoch() is the duration since epoch
    // which is converted to microseconds
    auto duration
        = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    return (static_cast<double>(duration));
};

/* ============================================================================================ */
/*  device query and print out their ID and name; return number of compute-capable devices. */
int64_t query_device_property()
{
    int                device_count;
    rocsparselt_status status = (rocsparselt_status)hipGetDeviceCount(&device_count);
    if(status != rocsparselt_status_success)
    {
        rocsparselt_cerr << "Query device error: cannot get device count" << std::endl;
        return -1;
    }
    else
    {
        rocsparselt_cout << "Query device success: there are " << device_count << " devices"
                         << std::endl;
    }

    for(int i = 0;; i++)
    {
        rocsparselt_cout
            << "-------------------------------------------------------------------------------"
            << std::endl;

        if(i >= device_count)
            break;

        hipDeviceProp_t    props;
        rocsparselt_status status = (rocsparselt_status)hipGetDeviceProperties(&props, i);
        if(status != rocsparselt_status_success)
        {
            rocsparselt_cerr << "Query device error: cannot get device ID " << i << "'s property"
                             << std::endl;
        }
        else
        {
            char buf[320];
            snprintf(
                buf,
                sizeof(buf),
                "Device ID %d : %s %s\n"
                "with %3.1f GB memory, max. SCLK %d MHz, max. MCLK %d MHz, compute capability "
                "%d.%d\n"
                "maxGridDimX %d, sharedMemPerBlock %3.1f KB, maxThreadsPerBlock %d, warpSize %d\n",
                i,
                props.name,
                props.gcnArchName,
                props.totalGlobalMem / 1e9,
                (int)(props.clockRate / 1000),
                (int)(props.memoryClockRate / 1000),
                props.major,
                props.minor,
                props.maxGridSize[0],
                props.sharedMemPerBlock / 1e3,
                props.maxThreadsPerBlock,
                props.warpSize);
            rocsparselt_cout << buf;
        }
    }

    return device_count;
}

/*  set current device to device_id */
void set_device(int64_t device_id)
{
    rocsparselt_status status = (rocsparselt_status)hipSetDevice(device_id);
    if(status != rocsparselt_status_success)
    {
        rocsparselt_cerr << "Set device error: cannot set device ID " << device_id
                         << ", there may not be such device ID" << std::endl;
    }
}

/*****************
 * local handles *
 *****************/

rocsparselt_local_handle::rocsparselt_local_handle()
{
    auto status = rocsparselt_init(&m_handle);
    if(status != rocsparselt_status_success)
        throw std::runtime_error(rocsparselt_status_to_string(status));

#ifdef GOOGLE_TEST
    if(t_set_stream_callback)
    {
        (*t_set_stream_callback)(m_handle);
        t_set_stream_callback.reset();
    }
#endif
}

rocsparselt_local_handle::rocsparselt_local_handle(const Arguments& arg)
    : rocsparselt_local_handle()
{

    // If the test specifies user allocated workspace, allocate and use it
    if(arg.user_allocated_workspace)
    {
        if((hipMalloc)(&m_memory, arg.user_allocated_workspace) != hipSuccess)
            throw std::bad_alloc();
    }

    // memory guard control, with multi-threading should not change values across threads
    d_vector_set_pad_length(arg.pad);
}

rocsparselt_local_handle::~rocsparselt_local_handle()
{
    if(m_memory)
        (hipFree)(m_memory);
    rocsparselt_destroy(&m_handle);
}
