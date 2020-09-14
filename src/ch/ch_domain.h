/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_domain.h: header file for domain manager's Cloud-Hypervisor driver functions
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

#include <glib-object.h>
#include "ch_conf.h"
#include "ch_monitor.h"
#include "virchrdev.h"
#include "vircgroup.h"
#include "logging/log_manager.h"

#define CH_DEV_VFIO "/dev/vfio/vfio"

/* Give up waiting for mutex after 30 seconds */
#define CH_JOB_WAIT_TIME (1000ull * 30)

/* Only 1 job is allowed at any time
 * A job includes *all* ch.so api, even those just querying
 * information, not merely actions */

enum virCHDomainJob {
    CH_JOB_NONE = 0,      /* Always set to 0 for easy if (jobActive) conditions */
    CH_JOB_QUERY,         /* Doesn't change any state */
    CH_JOB_DESTROY,       /* Destroys the domain (cannot be masked out) */
    CH_JOB_MODIFY,        /* May change state */
    CH_JOB_LAST
};
VIR_ENUM_DECL(virCHDomainJob);

typedef enum {
    CH_DOMAIN_LOG_CONTEXT_MODE_START,
    CH_DOMAIN_LOG_CONTEXT_MODE_ATTACH,
    CH_DOMAIN_LOG_CONTEXT_MODE_STOP,
} chDomainLogContextMode;


#define CH_TYPE_DOMAIN_LOG_CONTEXT ch_domain_log_context_get_type()
G_DECLARE_FINAL_TYPE(chDomainLogContext, ch_domain_log_context, CH, DOMAIN_LOG_CONTEXT, GObject);
struct _virCHDomainJobObj {
    virCond cond;                       /* Use to coordinate jobs */
    enum virCHDomainJob active;        /* Currently running job */
    int owner;                          /* Thread which set current job */
};

typedef struct _virCHDomainJobObj virCHDomainJobObj;

typedef struct _virCHDomainObjPrivate virCHDomainObjPrivate;
struct _virCHDomainObjPrivate {
    pid_t initpid;

    virCHDomainJobObj job;

    virCHDriver *driver;

    virCHMonitor *monitor;

    virChrdevs *devs;

    char *machineName;

    char *pidfile;

    virBitmap *autoNodeset;
    virBitmap *autoCpuset;

    virCgroup *cgroup;

    size_t tapfdSize;
    int *tapfd;

    virThread vmThreadWatcher;
    int vmThreadWatcherStop;

    char *ch_path;
};

#define CH_DOMAIN_PRIVATE(vm) \
    ((virCHDomainObjPrivate *) (vm)->privateData)

virCHMonitor *virCHDomainGetMonitor(virDomainObj *vm);

typedef struct _virCHDomainVcpuPrivate virCHDomainVcpuPrivate;
struct _virCHDomainVcpuPrivate {
    virObject parent;

    pid_t tid; /* vcpu thread id */
    virTristateBool halted;
};

#define CH_DOMAIN_VCPU_PRIVATE(vcpu) \
    ((virCHDomainVcpuPrivate *) (vcpu)->privateData)

extern virDomainXMLPrivateDataCallbacks virCHDriverPrivateDataCallbacks;
extern virDomainDefParserConfig virCHDriverDomainDefParserConfig;


chDomainLogContext *chDomainLogContextNew(virCHDriver *driver,
                                            virDomainObj *vm,
                                            chDomainLogContextMode mode);
int chDomainLogContextGetWriteFD(chDomainLogContext *ctxt);


int chDomainLogAppendMessage(virCHDriver *driver,
                               virDomainObj *vm,
                               const char *fmt,
                               ...) G_GNUC_PRINTF(3, 4);
int
virCHDomainObjBeginJob(virDomainObj *obj, enum virCHDomainJob job)
    G_GNUC_WARN_UNUSED_RESULT;

void

virCHDomainObjEndJob(virDomainObj *obj);

void
virCHDomainRemoveInactive(virCHDriver *driver,
                          virDomainObj *vm);

void
virCHDomainRemoveInactiveJob(virCHDriver *driver,
                             virDomainObj *vm);

void
virCHDomainRemoveInactiveJobLocked(virCHDriver *driver,
                                   virDomainObj *vm);

int virCHDomainRefreshThreadInfo(virDomainObj *vm);

pid_t virCHDomainGetVcpuPid(virDomainObj *vm, unsigned int vcpuid);
bool virCHDomainHasVcpuPids(virDomainObj *vm);

char *virCHDomainGetMachineName(virDomainObj *vm);

int
virCHDomainValidateVcpuInfo(virDomainObj *vm);

virDomainObj *virCHDomainObjFromDomain(virDomain *domain);

int
virCHDomainObjRestoreJob(virDomainObj *obj,
                         virCHDomainJobObj *job);
