/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_process.c: Process controller for Cloud-Hypervisor driver
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

#include <config.h>

#include <unistd.h>
#include <fcntl.h>
#include "datatypes.h"

#include "ch_cgroup.h"
#include "ch_domain.h"
#include "ch_hostdev.h"
#include "ch_interface.h"
#include "ch_monitor.h"
#include "ch_process.h"
#include "nwfilter_conf.h"
#include "virnuma.h"
#include "viralloc.h"
#include "virerror.h"
#include "viridentity.h"
#include "virjson.h"
#include "virlog.h"
#include "virpidfile.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_LOG_INIT("ch.ch_process");

#define START_SOCKET_POSTFIX ": starting up socket\n"
#define START_VM_POSTFIX ": starting up vm\n"



static virCHMonitor *
virCHProcessConnectMonitor(virCHDriver *driver,
                           virDomainObj *vm)
{
    virCHMonitor *monitor = NULL;

    monitor = virCHMonitorNew(vm, driver);

    return monitor;
}

static int
virCHProcessGetAllCpuAffinity(virBitmap **cpumapRet)
{
    *cpumapRet = NULL;

    if (!virHostCPUHasBitmap())
        return 0;

    if (!(*cpumapRet = virHostCPUGetOnlineBitmap()))
        return -1;

    return 0;
}

#if defined(HAVE_SCHED_GETAFFINITY) || defined(HAVE_BSD_CPU_AFFINITY)
static int
virCHProcessInitCpuAffinity(virDomainObj *vm)
{
    g_autoptr(virBitmap) cpumapToSet = NULL;
    virDomainNumatuneMemMode mem_mode;
    virCHDomainObjPrivate *priv = vm->privateData;

    if (!vm->pid) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Cannot setup CPU affinity until process is started"));
        return -1;
    }

    if (virDomainNumaGetNodeCount(vm->def->numa) <= 1 &&
        virDomainNumatuneGetMode(vm->def->numa, -1, &mem_mode) == 0 &&
        mem_mode == VIR_DOMAIN_NUMATUNE_MEM_STRICT) {
        virBitmap *nodeset = NULL;

        if (virDomainNumatuneMaybeGetNodeset(vm->def->numa,
                                             priv->autoNodeset,
                                             &nodeset,
                                             -1) < 0)
            return -1;

        if (virNumaNodesetToCPUset(nodeset, &cpumapToSet) < 0)
            return -1;
    } else if (vm->def->cputune.emulatorpin) {
        if (!(cpumapToSet = virBitmapNewCopy(vm->def->cputune.emulatorpin)))
            return -1;
    } else {
        if (virCHProcessGetAllCpuAffinity(&cpumapToSet) < 0)
            return -1;
    }

    if (cpumapToSet &&
        virProcessSetAffinity(vm->pid, cpumapToSet) < 0) {
        return -1;
    }

    return 0;
}
#else /* !defined(HAVE_SCHED_GETAFFINITY) && !defined(HAVE_BSD_CPU_AFFINITY) */
static int
virCHProcessInitCpuAffinity(virDomainObj *vm G_GNUC_UNUSED)
{
    return 0;
}
#endif /* !defined(HAVE_SCHED_GETAFFINITY) && !defined(HAVE_BSD_CPU_AFFINITY) */

/**
 * virCHProcessSetupPid:
 *
 * This function sets resource properties (affinity, cgroups,
 * scheduler) for any PID associated with a domain.  It should be used
 * to set up emulator PIDs as well as vCPU and I/O thread pids to
 * ensure they are all handled the same way.
 *
 * Returns 0 on success, -1 on error.
 */
static int
virCHProcessSetupPid(virDomainObj *vm,
                     pid_t pid,
                     virCgroupThreadName nameval,
                     int id,
                     virBitmap *cpumask,
                     unsigned long long period,
                     long long quota,
                     virDomainThreadSchedParam *sched)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    virDomainNumatuneMemMode mem_mode;
    virCgroup *cgroup = NULL;
    virBitmap *use_cpumask = NULL;
    virBitmap *affinity_cpumask = NULL;
    g_autoptr(virBitmap) hostcpumap = NULL;
    g_autofree char *mem_mask = NULL;
    int ret = -1;

    if ((period || quota) &&
        !virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        goto cleanup;
    }

    /* Infer which cpumask shall be used. */
    if (cpumask) {
        use_cpumask = cpumask;
    } else if (vm->def->placement_mode == VIR_DOMAIN_CPU_PLACEMENT_MODE_AUTO) {
        use_cpumask = priv->autoCpuset;
    } else if (vm->def->cpumask) {
        use_cpumask = vm->def->cpumask;
    } else {
        /* we can't assume cloud-hypervisor itself is running on all pCPUs,
         * so we need to explicitly set the spawned instance to all pCPUs. */
        if (virCHProcessGetAllCpuAffinity(&hostcpumap) < 0)
            goto cleanup;
        affinity_cpumask = hostcpumap;
    }

    /*
     * If CPU cgroup controller is not initialized here, then we need
     * neither period nor quota settings.  And if CPUSET controller is
     * not initialized either, then there's nothing to do anyway.
     */
    if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPU) ||
        virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {

        if (virDomainNumatuneGetMode(vm->def->numa, -1, &mem_mode) == 0 &&
            mem_mode == VIR_DOMAIN_NUMATUNE_MEM_STRICT &&
            virDomainNumatuneMaybeFormatNodeset(vm->def->numa,
                                                priv->autoNodeset,
                                                &mem_mask, -1) < 0)
            goto cleanup;

        if (virCgroupNewThread(priv->cgroup, nameval, id, true, &cgroup) < 0)
            goto cleanup;

        if (virCgroupHasController(priv->cgroup, VIR_CGROUP_CONTROLLER_CPUSET)) {
            if (use_cpumask &&
                chSetupCgroupCpusetCpus(cgroup, use_cpumask) < 0)
                goto cleanup;

            if (mem_mask && virCgroupSetCpusetMems(cgroup, mem_mask) < 0)
                goto cleanup;

        }

        if ((period || quota) &&
            chSetupCgroupVcpuBW(cgroup, period, quota) < 0)
            goto cleanup;

        /* Move the thread to the sub dir */
        VIR_DEBUG("Adding pid %d to cgroup", pid);
        if (virCgroupAddThread(cgroup, pid) < 0)
            goto cleanup;

    }

    if (!affinity_cpumask)
        affinity_cpumask = use_cpumask;

    /* Setup legacy affinity. */
    if (affinity_cpumask &&
        virProcessSetAffinity(pid, affinity_cpumask,
            affinity_cpumask == hostcpumap) < 0)
        goto cleanup;

    /* Set scheduler type and priority, but not for the main thread. */
    if (sched &&
        nameval != VIR_CGROUP_THREAD_EMULATOR &&
        virProcessSetScheduler(pid, sched->policy, sched->priority) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    if (cgroup) {
        if (ret < 0)
            virCgroupRemove(cgroup);
        virCgroupFree(cgroup);
    }

    return ret;
}

int
virCHProcessSetupIOThread(virDomainObj *vm,
                         virDomainIOThreadInfo *iothread)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    return virCHProcessSetupPid(vm, iothread->iothread_id,
                               VIR_CGROUP_THREAD_IOTHREAD,
                               iothread->iothread_id,
                               priv->autoCpuset, // This should be updated when CLH supports accepting
                                     // iothread settings from input domain definition
                               vm->def->cputune.iothread_period,
                               vm->def->cputune.iothread_quota,
                               NULL); // CLH doesn't allow choosing a scheduler for iothreads.
}

static int
virCHProcessSetupIOThreads(virDomainObj *vm)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    virDomainIOThreadInfo **iothreads = NULL;
    size_t i;
    size_t  niothreads;

    niothreads = virCHMonitorGetIOThreads(priv->monitor, &iothreads);
    for (i = 0; i < niothreads; i++) {
        VIR_DEBUG("IOThread index = %ld , tid = %d", i, iothreads[i]->iothread_id);
        if (virCHProcessSetupIOThread(vm, iothreads[i]) < 0)
            return -1;
    }
    return 0;
}


int
virCHProcessSetupEmulatorThread(virDomainObj *vm, pid_t tid)
{
    return virCHProcessSetupPid(vm, tid, VIR_CGROUP_THREAD_EMULATOR,
                               0, vm->def->cputune.emulatorpin,
                               vm->def->cputune.emulator_period,
                               vm->def->cputune.emulator_quota,
                               vm->def->cputune.emulatorsched);
}

static int
virCHProcessSetupEmulatorThreads(virDomainObj *vm)
{
    int i=0;
    virCHDomainObjPrivate *priv = vm->privateData;
    /*
    Cloud-hypervisor start 4 Emulator threads by default:
    vmm
    cloud-hypervisor
    http-server
    signal_handler
    */
    for (i=0; i < priv->monitor->nthreads; i++) {
        if (priv->monitor->threads[i].type == virCHThreadTypeEmulator) {
            VIR_DEBUG("Setup tid = %d (%s) Emulator thread", priv->monitor->threads[i].tid,
                priv->monitor->threads[i].emuInfo.thrName);

            if (virCHProcessSetupEmulatorThread(vm, priv->monitor->threads[i].tid) < 0)
                return -1;
        }
    }
    return 0;
}

static void
virCHProcessUpdateConsoleDevice(virDomainObj *vm,
                                virJSONValue *config,
                                const char *device)
{
    const char *path;
    virDomainChrDef *chr = NULL;
    virJSONValue *dev, *file;

    if (!config)
        return;

    dev = virJSONValueObjectGet(config, device);
    if (!dev)
        return;

    file = virJSONValueObjectGet(dev, "file");
    if (!file)
        return;

    path = virJSONValueGetString(file);
    if (!path)
        return;

    if (STREQ(device, "console")) {
        chr = vm->def->consoles[0];
    } else if (STREQ(device, "serial")) {
        chr = vm->def->serials[0];
    }

    if (chr && chr->source)
        chr->source->data.file.path = g_strdup(path);
}

static void
virCHProcessUpdateConsole(virDomainObj *vm,
                          virJSONValue *info)
{
    virJSONValue *config;

    config = virJSONValueObjectGet(info, "config");
    if (!config)
        return;

    virCHProcessUpdateConsoleDevice(vm, config, "console");
    virCHProcessUpdateConsoleDevice(vm, config, "serial");
}

static void
virCHProcessUpdateStatus(virDomainObj *vm,
                         virJSONValue *info)
{
    virJSONValue *state;
    const char *value;

    state = virJSONValueObjectGet(info, "state");
    if (!state)
        return;

    value = virJSONValueGetString(state);

    if (STREQ(value, "Created")) {
        virDomainObjSetState(vm, VIR_DOMAIN_NOSTATE, 0);
    } else if (STREQ(value, "Running")) {
        virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, 0);
    } else if (STREQ(value, "Shutdown")) {
        virDomainObjSetState(vm, VIR_DOMAIN_SHUTDOWN, 0);
    } else if (STREQ(value, "Paused")) {
        virDomainObjSetState(vm, VIR_DOMAIN_PMSUSPENDED, 0);
    }
}

static int
virCHProcessUpdateInfo(virDomainObj *vm)
{
    virJSONValue *info;
    virCHDomainObjPrivate *priv = vm->privateData;
    if (virCHMonitorGetInfo(priv->monitor, &info) < 0)
        return -1;

    virCHProcessUpdateStatus(vm, info);

    virCHProcessUpdateConsole(vm, info);

    virJSONValueFree(info);

    return 0;
}

/**
 * virCHProcessSetupVcpu:
 * @vm: domain object
 * @vcpuid: id of VCPU to set defaults
 *
 * This function sets resource properties (cgroups, affinity, scheduler) for a
 * vCPU. This function expects that the vCPU is online and the vCPU pids were
 * correctly detected at the point when it's called.
 *
 * Returns 0 on success, -1 on error.
 */
int
virCHProcessSetupVcpu(virDomainObj *vm,
                      unsigned int vcpuid)
{
    pid_t vcpupid = virCHDomainGetVcpuPid(vm, vcpuid);
    virDomainVcpuDef *vcpu = virDomainDefGetVcpu(vm->def, vcpuid);

    return virCHProcessSetupPid(vm, vcpupid, VIR_CGROUP_THREAD_VCPU,
                                vcpuid, vcpu->cpumask,
                                vm->def->cputune.period,
                                vm->def->cputune.quota,
                                &vcpu->sched);
}

static int
virCHProcessSetupVcpuPids(virDomainObj *vm)
{
    virCHMonitorThreadInfo *info = NULL;
    size_t nthreads, ncpus = 0;
    size_t i;

    nthreads = virCHMonitorGetThreadInfo(virCHDomainGetMonitor(vm),
                                         false, &info);
    for (i = 0; i < nthreads; i++) {
        virCHDomainVcpuPrivate *vcpupriv;
        virDomainVcpuDef *vcpu;
        virCHMonitorCPUInfo *vcpuInfo;

        if (info[i].type != virCHThreadTypeVcpu)
            continue;

        vcpuInfo = &info[i].vcpuInfo;
        vcpu = virDomainDefGetVcpu(vm->def, vcpuInfo->cpuid);
        vcpupriv = CH_DOMAIN_VCPU_PRIVATE(vcpu);
        vcpupriv->tid = info[i].tid;
        ncpus++;
    }

    return 0;
}

/*
 * Sets up vcpu's affinity, quota limits etc.
 * Assumes that vm alread has all the vcpu pid
 * information(virCHMonitorRefreshThreadInfo has been
 * called before this function)
 */
static int
virCHProcessSetupVcpus(virDomainObj *vm)
{
    virDomainVcpuDef *vcpu;
    unsigned int maxvcpus = virDomainDefGetVcpusMax(vm->def);
    size_t i;

    if ((vm->def->cputune.period || vm->def->cputune.quota) &&
        !virCgroupHasController(((virCHDomainObjPrivate *) vm->privateData)->cgroup,
                                VIR_CGROUP_CONTROLLER_CPU)) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("cgroup cpu is required for scheduler tuning"));
        return -1;
    }

    virCHProcessSetupVcpuPids(vm);

    if (!virCHDomainHasVcpuPids(vm)) {
        /* If any CPU has custom affinity that differs from the
         * VM default affinity, we must reject it */
        for (i = 0; i < maxvcpus; i++) {
            vcpu = virDomainDefGetVcpu(vm->def, i);

            if (!vcpu->online)
                continue;

            if (vcpu->cpumask &&
                !virBitmapEqual(vm->def->cpumask, vcpu->cpumask)) {
                virReportError(VIR_ERR_OPERATION_INVALID, "%s",
                                _("cpu affinity is not supported"));
                return -1;
            }
        }

        return 0;
    }

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);

        if (!vcpu->online)
            continue;

        if (virCHProcessSetupVcpu(vm, i) < 0)
            return -1;
    }

    return 0;
}

int virCHProcessSetupThreads(virDomainObj *vm)
{
    virCHDriver *driver = CH_DOMAIN_PRIVATE(vm)->driver;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    virCHDomainObjPrivate *priv = vm->privateData;
    int ret;

    ret = virCHMonitorRefreshThreadInfo(priv->monitor);
    if (ret <= 0)
        return ret;

    VIR_DEBUG("Setting emulator tuning/settings");
    ret = virCHProcessSetupEmulatorThreads(vm);

    if (!ret) {
        VIR_DEBUG("Setting iothread tuning/settings");
        ret = virCHProcessSetupIOThreads(vm);
    }

    if (!ret) {
        VIR_DEBUG("Setting vCPU tuning/settings");
        ret = virCHProcessSetupVcpus(vm);
    }

    if (!ret) {
        ret = virDomainObjSave(vm, driver->xmlopt, cfg->stateDir);
    }

    return ret;
}

/**
 * chProcessNetworkPrepareDevices
 */
static int
chProcessNetworkPrepareDevices(virCHDriver *driver, virDomainObj *vm)
{
    virDomainDef *def = vm->def;
    size_t i;
    virCHDomainObjPrivate *priv = vm->privateData;
    g_autoptr(virConnect) conn = NULL;
    int *tapfd = NULL;
    size_t tapfdSize = 0;

    for (i = 0; i < def->nnets; i++) {
        virDomainNetDef *net = def->nets[i];
        virDomainNetType actualType;

        /* If appropriate, grab a physical device from the configured
         * network's pool of devices, or resolve bridge device name
         * to the one defined in the network definition.
         */
        if (net->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
            if (!conn && !(conn = virGetConnectNetwork())) {
                return -1;
            }
            if (virDomainNetAllocateActualDevice(conn, def, net) < 0) {
                return -1;
            }
        }

        actualType = virDomainNetGetActualType(net);
        if (actualType == VIR_DOMAIN_NET_TYPE_HOSTDEV &&
            net->type == VIR_DOMAIN_NET_TYPE_NETWORK) {
            /* Each type='hostdev' network device must also have a
             * corresponding entry in the hostdevs array. For netdevs
             * that are hardcoded as type='hostdev', this is already
             * done by the parser, but for those allocated from a
             * network / determined at runtime, we need to do it
             * separately.
             */
            virDomainHostdevDef *hostdev = virDomainNetGetActualHostdev(net);
            virDomainHostdevSubsysPCI *pcisrc = &hostdev->source.subsys.u.pci;

            if (virDomainHostdevFind(def, hostdev, NULL) >= 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("PCI device %04x:%02x:%02x.%x "
                                 "allocated from network %s is already "
                                 "in use by domain %s"),
                               pcisrc->addr.domain, pcisrc->addr.bus,
                               pcisrc->addr.slot, pcisrc->addr.function,
                               net->data.network.name, def->name);
                return -1;
            }
            if (virDomainHostdevInsert(def, hostdev) < 0)
                return -1;
        } else if (actualType == VIR_DOMAIN_NET_TYPE_USER ) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
              _("VIR_DOMAIN_NET_TYPE_USER is not a supported Network type"));
        } else if (actualType == VIR_DOMAIN_NET_TYPE_NETWORK) {
            tapfdSize = net->driver.virtio.queues;
            if (!tapfdSize)
                tapfdSize = 1;
            VIR_ERROR("tapfd: %lu", tapfdSize);

            tapfd = g_new(int, tapfdSize);

            memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

            if (chInterfaceBridgeConnect(def, driver,  net,
                                       tapfd, &tapfdSize) < 0)
                goto cleanup;

            // Store tap information in Private Data
            // This info will be used while generating Network Json
            priv->tapfd = g_steal_pointer(&tapfd);
            priv->tapfdSize = tapfdSize;
        } else if (actualType == VIR_DOMAIN_NET_TYPE_ETHERNET) {
            tapfdSize = net->driver.virtio.queues;
            if (!tapfdSize)
                tapfdSize = 1;

            tapfd = g_new(int, tapfdSize);

            memset(tapfd, -1, tapfdSize * sizeof(tapfd[0]));

            if (chInterfaceEthernetConnect(def, driver, net,
                                           tapfd, tapfdSize) < 0)
                goto cleanup;

            // Store tap information in Private Data
            // This info will be used while generating Network Json
            priv->tapfd = g_steal_pointer(&tapfd);
            priv->tapfdSize = tapfdSize;
        }
    }

    return 0;

 cleanup:
    g_free(tapfd);
    return -1;
}


/**
 * virCHProcessStart:
 * @driver: pointer to driver structure
 * @vm: pointer to virtual machine structure
 * @reason: reason for switching vm to running state
 *
 * Starts Cloud-Hypervisor listen on a local socket
 *
 * Returns 0 on success or -1 in case of error
 */
int virCHProcessStart(virCHDriver *driver,
                      virDomainObj *vm,
                      virDomainRunningReason reason)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    g_autofree int *nicindexes = NULL;
    size_t nnicindexes = 0;
    int ret = -1;

    /* network devices must be "prepared" before hostdevs, because
     * setting up a network device might create a new hostdev that
     * will need to be setup.
     */
    VIR_DEBUG("Preparing network devices");
    if (chProcessNetworkPrepareDevices(driver, vm) < 0)
        goto cleanup;

    /* Bring up netdevs before starting CPUs */
    if (chInterfaceStartDevices(vm->def) < 0)
       return -1;

    VIR_DEBUG("Preparing host devices");
    if (chHostdevPrepareDomainDevices(driver, vm->def,
                                      VIR_HOSTDEV_COLD_BOOT) < 0)
        goto cleanup;

    if (!priv->monitor) {
        /* And we can get the first monitor connection now too */
        if (!(priv->monitor = virCHProcessConnectMonitor(driver, vm))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to create connection to CH socket"));
            goto cleanup;
        }

        if (virCHMonitorCreateVM(priv->monitor,
                                 &nnicindexes, &nicindexes) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                        _("failed to create guest VM"));
            goto cleanup;
        }
    }

    vm->def->id = vm->pid;
    priv->machineName = virCHDomainGetMachineName(vm);

    if (chSetupCgroup(vm, nnicindexes, nicindexes) < 0)
        goto cleanup;

    if (virCHProcessInitCpuAffinity(vm) < 0)
        goto cleanup;

    if (virCHMonitorBootVM(priv->monitor) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to boot guest VM"));
        goto cleanup;
    }

    virCHMonitorRefreshThreadInfo(priv->monitor);

    virCHProcessUpdateInfo(vm);

    if (virCHProcessSetupThreads(vm) < 0)
        goto cleanup;

    VIR_DEBUG("Setting global CPU cgroup (if required)");
    if (chSetupGlobalCpuCgroup(vm) < 0)
        goto cleanup;

    virDomainObjSetState(vm, VIR_DOMAIN_RUNNING, reason);

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        goto cleanup;

    return 0;

 cleanup:
    if (ret)
        virCHProcessStop(driver, vm, VIR_DOMAIN_SHUTOFF_FAILED);

    return ret;
}

static int
virCHConnectMonitor(virCHDriver *driver, virDomainObj *vm)
{
    virCHDomainObjPrivate *priv = vm->privateData;
    virCHMonitor *mon = NULL;

    mon = virCHMonitorOpen(vm, driver);

    priv->monitor = mon;

    if (priv->monitor == NULL) {
        VIR_INFO("Failed to connect monitor for %s", vm->def->name);
        return -1;
    }

    return 0;
}

static int
virCHProcessUpdateState(virDomainObj *vm)
{
    if (virCHProcessUpdateInfo(vm) < 0)
        return -1;

    return 0;
}

struct virCHProcessReconnectData {
    virCHDriver *driver;
    virDomainObj *obj;
    virIdentity *identity;
};
/*
 * Open an existing VM's monitor, re-detect VCPU threads
 *
 * This function also inherits a locked and ref'd domain object.
 *
 * This function needs to:
 * 1. Enter job
 * 2. reconnect to monitor
 * 4. continue reconnect process
 * 5. EndJob
 *
 */
static void
virCHProcessReconnect(void *opaque)
{
    struct virCHProcessReconnectData *data = opaque;
    virCHDriver *driver = data->driver;
    virDomainObj *obj = data->obj;
    virCHDomainObjPrivate *priv;
    virCHDomainJobObj oldjob;
    int state;
    int reason;
    g_autoptr(virCHDriverConfig) cfg = NULL;
    bool jobStarted = false;

    virIdentitySetCurrent(data->identity);
    g_clear_object(&data->identity);
    g_free(data);

    virCHDomainObjRestoreJob(obj, &oldjob);

    cfg = virCHDriverGetConfig(driver);
    priv = obj->privateData;

    if (virCHDomainObjBeginJob(obj, CH_JOB_MODIFY) < 0)
        goto error;
    jobStarted = true;

    if (chHostdevUpdateActiveDomainDevices(driver, obj->def) < 0)
        goto error;

    if (virCHConnectMonitor(driver, obj) < 0)
        goto error;

    obj->def->id = obj->pid;
    VIR_DEBUG("Domain Object def->id = %d", obj->def->id);

    priv->machineName = virCHDomainGetMachineName(obj);
    if (!priv->machineName)
        goto error;

    if (chConnectCgroup(obj) < 0)
        goto error;

    if (virCHProcessUpdateState(obj) < 0)
        goto error;

    state = virDomainObjGetState(obj, &reason);

    /* In case the domain shutdown while we were not running,
     * we need to finish the shutdown process.
     */
    if (state == VIR_DOMAIN_SHUTDOWN ||
        (state == VIR_DOMAIN_PAUSED &&
         reason == VIR_DOMAIN_PAUSED_SHUTTING_DOWN)) {
        VIR_DEBUG("Finishing shutdown sequence for domain %s",
                  obj->def->name);
        virCHProcessStop(driver, obj, VIR_DOMAIN_SHUTOFF_DAEMON);
        goto cleanup;
    }

    /* update domain state XML with possibly updated state in virDomainObj */
    if (virDomainObjSave(obj, driver->xmlopt, cfg->stateDir) < 0)
        goto error;

 cleanup:
    if (jobStarted) {
        if (!virDomainObjIsActive(obj))
            virCHDomainRemoveInactive(driver, obj);
        virCHDomainObjEndJob(obj);
    } else {
        if (!virDomainObjIsActive(obj))
            virCHDomainRemoveInactiveJob(driver, obj);
    }
    virDomainObjEndAPI(&obj);
    virIdentitySetCurrent(NULL);
    return;

 error:
    if (virDomainObjIsActive(obj)) {
        /* We can't get the monitor back, so must kill the VM
         * to remove danger of it ending up running twice if
         * user tries to start it again later.
         * If BeginJob failed, we jumped here without a job, let's hope another
         * thread didn't have a chance to start playing with the domain yet
         * (it's all we can do anyway).
         */
        virCHProcessStop(driver, obj, VIR_DOMAIN_SHUTOFF_UNKNOWN);
    }
    goto cleanup;
}

static int
chProcessReconnectHelper(virDomainObj *obj,
                         void *opaque)
{
    virThread thread;
    struct virCHProcessReconnectData *src = opaque;
    struct virCHProcessReconnectData *data;
    g_autofree char *name = NULL;

    /* If the VM was inactive, we don't need to reconnect */
    if (!obj->pid)
        return 0;

    data = g_new0(struct virCHProcessReconnectData, 1);

    memcpy(data, src, sizeof(*data));
    data->obj = obj;
    data->identity = virIdentityGetCurrent();

    virNWFilterReadLockFilterUpdates();

    /* this lock and reference will be eventually transferred to the thread
     * that handles the reconnect */
    virObjectLock(obj);
    virObjectRef(obj);

    name = g_strdup_printf("init-%s", obj->def->name);

    if (virThreadCreateFull(&thread, false, virCHProcessReconnect,
                            name, false, data) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create thread. CH initialization "
                         "might be incomplete"));
        /* We can't spawn a thread, kill ch.
         * It's safe to call virCHProcessStop without a job here since there
         * is no thread that could be doing anything else with the same domain
         * object.
         */
        virCHProcessStop(src->driver, obj, VIR_DOMAIN_SHUTOFF_FAILED);
        virCHDomainRemoveInactiveJobLocked(src->driver, obj);

        virDomainObjEndAPI(&obj);
        virNWFilterUnlockFilterUpdates();
        g_clear_object(&data->identity);
        g_free(data);
        return -1;
    }

    return 0;
}

/**
 * chProcessReconnectAll
 *
 * Try to re-open the resources for live VMs that we care
 * about.
 */
void
chProcessReconnectAll(virCHDriver *driver)
{
    struct virCHProcessReconnectData data = {.driver = driver};
    virDomainObjListForEach(driver->domains, true,
                            chProcessReconnectHelper, &data);
}

/**
 * chProcessRemoveDomainStatus
 *
 * remove all state files of a domain from statedir
 */
static void
chProcessRemoveDomainStatus(virCHDriver *driver,
                            virDomainObj *vm)
{
    g_autofree char *file = NULL;
    virCHDomainObjPrivate *priv = vm->privateData;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);

    file = g_strdup_printf("%s/%s.xml", cfg->stateDir, vm->def->name);

    if (unlink(file) < 0 && errno != ENOENT && errno != ENOTDIR)
        VIR_WARN("Failed to remove domain XML for %s: %s",
                 vm->def->name, g_strerror(errno));

    if (priv->pidfile &&
        unlink(priv->pidfile) < 0 &&
        errno != ENOENT)
        VIR_WARN("Failed to remove PID file for %s: %s",
                 vm->def->name, g_strerror(errno));
}

int virCHProcessStop(virCHDriver *driver,
                     virDomainObj *vm,
                     virDomainShutoffReason reason)
{
    int ret;
    int retries = 0;
    virCHDomainObjPrivate *priv = vm->privateData;
    VIR_DEBUG("Stopping VM name=%s pid=%d reason=%d",
              vm->def->name, (int)vm->pid, (int)reason);

    if (priv->monitor) {
        virCHMonitorClose(priv->monitor);
        priv->monitor = NULL;
    }

    chHostdevReAttachDomainDevices(driver, vm->def);

retry:
    if ((ret = chRemoveCgroup(vm)) < 0) {
        if (ret == -EBUSY && (retries++ < 5)) {
            g_usleep(200*1000);
            goto retry;
        }
        VIR_WARN("Failed to remove cgroup for %s",
                 vm->def->name);
    }

    vm->pid = -1;
    vm->def->id = -1;
    chProcessRemoveDomainStatus(driver, vm);

    virDomainObjSetState(vm, VIR_DOMAIN_SHUTOFF, reason);

    return 0;
}
