/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_monitor.h: header file for managing Cloud-Hypervisor interactions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <curl/curl.h>

#include "ch_conf.h"
#include "virobject.h"
#include "virjson.h"
#include "domain_conf.h"

#define URL_ROOT "http://localhost/api/v1"
#define URL_VMM_SHUTDOWN "vmm.shutdown"
#define URL_VM_CREATE "vm.create"
#define URL_VM_RESIZE "vm.resize"
#define URL_VM_DELETE "vm.delete"
#define URL_VM_BOOT "vm.boot"
#define URL_VM_SHUTDOWN "vm.shutdown"
#define URL_VM_REBOOT "vm.reboot"
#define URL_VM_Suspend "vm.pause"
#define URL_VM_RESUME "vm.resume"
#define URL_VM_INFO "vm.info"

#define VIRCH_THREAD_NAME_LEN   16

typedef enum {
    virCHThreadTypeEmulator,
    virCHThreadTypeVcpu,
    virCHThreadTypeIO,
    virCHThreadTypeUnknown,
    virCHThreadTypeMax
} virCHThreadType;

typedef struct _virCHMonitorCPUInfo virCHMonitorCPUInfo;

struct _virCHMonitorCPUInfo {
    int cpuid;

    bool online;
};

typedef struct _virCHMonitorEmuThreadInfo virCHMonitorEmuThreadInfo;

struct _virCHMonitorEmuThreadInfo {
    char    thrName[VIRCH_THREAD_NAME_LEN];
};

typedef struct _virCHMonitorIOThreadInfo virCHMonitorIOThreadInfo;

struct _virCHMonitorIOThreadInfo {
    char    thrName[VIRCH_THREAD_NAME_LEN];
};

typedef struct _virCHMonitorThreadInfo virCHMonitorThreadInfo;

struct _virCHMonitorThreadInfo {
    virCHThreadType type;
    pid_t   tid;

    union {
        virCHMonitorCPUInfo vcpuInfo;
        virCHMonitorEmuThreadInfo emuInfo;
        virCHMonitorIOThreadInfo ioInfo;
    };
};

typedef enum {
    /* source: vmm */
    virCHMonitorVmmEventStarting = 0,
    virCHMonitorVmmEventShutdown,

    /* source: vm */
    virCHMonitorVmEventBooting,
    virCHMonitorVmEventBooted,
    virCHMonitorVmEventPausing,
    virCHMonitorVmEventPaused,
    virCHMonitorVmEventResuming,
    virCHMonitorVmEventResumed,
    virCHMonitorVmEventSnapshotting,
    virCHMonitorVmEventSnapshotted,
    virCHMonitorVmEventRestoring,
    virCHMonitorVmEventRestored,
    virCHMonitorVmEventResizing,
    virCHMonitorVmEventResized,
    virCHMonitorVmEventShutdown,
    virCHMonitorVmEventDeleted,

    /* source: cpu_manager */
    virCHMonitorCpuCreateVcpu,

    /* source: virtio-device */
    virCHMonitorVirtioDeviceEventActivated,
    virCHMonitorVirtioDeviceEventReset,

    virCHMonitorEventMax
} virCHMonitorEvent;

/*
 * Size of the buffer used to read the
 * monitor events. Hard coded to max
 * size of the pipe.
 */
#define CH_MONITOR_BUFFER_SZ    PIPE_BUF

typedef struct _virCHMonitor virCHMonitor;

struct _virCHMonitor {
    virObjectLockable parent;

    CURL *handle;

    char *socketpath;

    char *monitorpath;

    // Buffer to hold the data read from pipe
    char *buffer;
    // Offset to which new data from pipe has to be read.
    size_t buf_offset;
    // Size of the data read from pipe in buffer
    size_t buf_fill_sz;

    virThread event_loop_thread;
    int event_loop_stop;

    pid_t pid;

    virDomainObj *vm;

    size_t nthreads;
    virCHMonitorThreadInfo *threads;
};

virCHMonitor *virCHMonitorOpen(virDomainObj *vm, virCHDriver *driver);
virCHMonitor *virCHMonitorNew(virDomainObj *vm, virCHDriver *driver);
void virCHMonitorClose(virCHMonitor *mon);

int virCHMonitorCreateVM(virCHMonitor *mon,
                         size_t *nnicindexes,
                         int **nicindexes);

int virCHMonitorBootVM(virCHMonitor *mon);
int virCHMonitorShutdownVM(virCHMonitor *mon);
int virCHMonitorRebootVM(virCHMonitor *mon);
int virCHMonitorSuspendVM(virCHMonitor *mon);
int virCHMonitorResumeVM(virCHMonitor *mon);

ssize_t virCHMonitorRefreshThreadInfo(virCHMonitor *mon);
size_t virCHMonitorGetThreadInfo(virCHMonitor *mon,
                                 bool refresh,
                                 virCHMonitorThreadInfo **threads);

int virCHMonitorGetIOThreads(virCHMonitor *mon,
                             virDomainIOThreadInfo ***iothreads);

int virCHMonitorGetInfo(virCHMonitor *mon, virJSONValue **info);

int
virCHMonitorResizeCPU(virCHMonitor *mon,
                      unsigned int nvcpus);
