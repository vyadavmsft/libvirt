/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_monitor.c: Manage Cloud-Hypervisor interactions
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

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>

#include <curl/curl.h>

#include "ch_conf.h"
#include "ch_domain.h"
#include "ch_monitor.h"
#include "ch_process.h"
#include "viralloc.h"
#include "vircommand.h"
#include "virerror.h"
#include "virfile.h"
#include "virjson.h"
#include "virlog.h"
#include "virstring.h"
#include "virpidfile.h"
#include "virtime.h"
#include "ch_interface.h"

#define VIR_FROM_THIS VIR_FROM_CH

/*
 * Retry count for temporary failures observed
 * in Monitor code path.
 */
#define MONITOR_TMP_FAIL_RETRIES 5

VIR_LOG_INIT("ch.ch_monitor");

static virClass *virCHMonitorClass;
static void virCHMonitorDispose(void *obj);

static void virCHMonitorThreadInfoFree(virCHMonitor *mon);

static int virCHMonitorOnceInit(void)
{
    if (!VIR_CLASS_NEW(virCHMonitor, virClassForObjectLockable()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virCHMonitor);

int virCHMonitorShutdownVMM(virCHMonitor *mon);
int virCHMonitorPutNoContent(virCHMonitor *mon, const char *endpoint);
int virCHMonitorGet(virCHMonitor *mon, const char *endpoint, virJSONValue **reponse);

static int
virCHMonitorBuildCPUJson(virJSONValue *content, virDomainDef *vmdef)
{
    virJSONValue *cpus;
    unsigned int maxvcpus = 0;
    unsigned int nvcpus = 0;
    virDomainVcpuDef *vcpu;
    size_t i;

    /* count maximum allowed number vcpus and enabled vcpus when boot.*/
    maxvcpus = virDomainDefGetVcpusMax(vmdef);
    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vmdef, i);
        if (vcpu->online)
            nvcpus++;
    }

    if (maxvcpus != 0 || nvcpus != 0) {
        cpus = virJSONValueNewObject();
        if (virJSONValueObjectAppendNumberInt(cpus, "boot_vcpus", nvcpus) < 0)
            goto cleanup;
        if (virJSONValueObjectAppendNumberInt(cpus, "max_vcpus", vmdef->maxvcpus) < 0)
            goto cleanup;
        if (virJSONValueObjectAppend(content, "cpus", &cpus) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(cpus);
    return -1;
}

static int
virCHMonitorBuildPTYJson(virJSONValue *content, virDomainDef *vmdef)
{
    virJSONValue *ptyc = virJSONValueNewObject();
    virJSONValue *ptys = virJSONValueNewObject();

    if (vmdef->nconsoles || vmdef->nserials) {
        if ((vmdef->nconsoles &&
             vmdef->consoles[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)
            && (vmdef->nserials &&
                vmdef->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only a single console or serial can be configured for this domain"));
            return -1;
        } else if (vmdef->nconsoles > 1) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only a single console can be configured for this domain"));
            return -1;
        } else if (vmdef->nserials > 1) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Only a single serial can be configured for this domain"));
            return -1;
        }
    }

    if (vmdef->nconsoles && vmdef->consoles[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY) {
        if (virJSONValueObjectAppendString(ptyc, "mode", "Pty") < 0)
            goto cleanup;
    } else {
        if (virJSONValueObjectAppendString(ptyc, "mode", "Null") < 0)
            goto cleanup;
    }

    if (vmdef->nserials && vmdef->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY) {
        if (virJSONValueObjectAppendString(ptys, "mode", "Pty") < 0)
            goto cleanup;
    } else {
        if (virJSONValueObjectAppendString(ptys, "mode", "Null") < 0)
            goto cleanup;
    }

    if (virJSONValueObjectAppend(content, "console", &ptyc) < 0)
        goto cleanup;
    if (virJSONValueObjectAppend(content, "serial", &ptys) < 0)
        goto cleanup;

    return 0;

 cleanup:
    virJSONValueFree(ptyc);
    virJSONValueFree(ptys);
    return -1;
}

static int
virCHMonitorBuildKernelRelatedJson(virJSONValue *content, virDomainDef *vmdef)
{
    virJSONValue *kernel = virJSONValueNewObject();
    virJSONValue *cmdline = virJSONValueNewObject();
    virJSONValue *initramfs = virJSONValueNewObject();

    if (vmdef->os.kernel == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Kernel image path in this domain is not defined"));
        goto cleanup;
    } else {
        kernel = virJSONValueNewObject();
        if (virJSONValueObjectAppendString(kernel, "path", vmdef->os.kernel) < 0)
            goto cleanup;
        if (virJSONValueObjectAppend(content, "kernel", &kernel) < 0)
            goto cleanup;
    }

    if (vmdef->os.cmdline) {
        if (virJSONValueObjectAppendString(cmdline, "args", vmdef->os.cmdline) < 0)
            goto cleanup;
        if (virJSONValueObjectAppend(content, "cmdline", &cmdline) < 0)
            goto cleanup;
    }

    if (vmdef->os.initrd != NULL) {
        if (virJSONValueObjectAppendString(initramfs, "path", vmdef->os.initrd) < 0)
            goto cleanup;
        if (virJSONValueObjectAppend(content, "initramfs", &initramfs) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(kernel);
    virJSONValueFree(cmdline);
    virJSONValueFree(initramfs);

    return -1;
}

static int
virCHMonitorBuildMemoryJson(virJSONValue *content, virDomainDef *vmdef)
{
    virJSONValue *memory;
    unsigned long long total_memory = virDomainDefGetMemoryInitial(vmdef) * 1024;

    if (total_memory != 0) {
        memory = virJSONValueNewObject();
        if (virJSONValueObjectAppendNumberUlong(memory, "size", total_memory) < 0)
            goto cleanup;
        if (virJSONValueObjectAppend(content, "memory", &memory) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(memory);
    return -1;
}

static int
virCHMonitorBuildDiskJson(virJSONValue *disks, virDomainDiskDef *diskdef)
{
    virJSONValue *disk = virJSONValueNewObject();

    if (!diskdef->src)
        goto cleanup;

    switch (diskdef->src->type) {
    case VIR_STORAGE_TYPE_FILE:
        if (!diskdef->src->path) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("Missing disk file path in domain"));
            goto cleanup;
        }
        if (diskdef->bus != VIR_DOMAIN_DISK_BUS_VIRTIO) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("Only virtio bus types are supported for '%s'"), diskdef->src->path);
            goto cleanup;
        }
        if (virJSONValueObjectAppendString(disk, "path", diskdef->src->path) < 0)
            goto cleanup;
        if (diskdef->src->readonly) {
            if (virJSONValueObjectAppendBoolean(disk, "readonly", true) < 0)
                goto cleanup;
        }
        if (virJSONValueArrayAppend(disks, &disk) < 0)
            goto cleanup;

        break;
    case VIR_STORAGE_TYPE_NONE:
    case VIR_STORAGE_TYPE_BLOCK:
    case VIR_STORAGE_TYPE_DIR:
    case VIR_STORAGE_TYPE_NETWORK:
    case VIR_STORAGE_TYPE_VOLUME:
    case VIR_STORAGE_TYPE_NVME:
    case VIR_STORAGE_TYPE_VHOST_USER:
    default:
        virReportEnumRangeError(virStorageType, diskdef->src->type);
        goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(disk);
    return -1;
}

static int
virCHMonitorBuildDisksJson(virJSONValue *content, virDomainDef *vmdef)
{
    virJSONValue *disks;
    size_t i;

    if (vmdef->ndisks > 0) {
        disks = virJSONValueNewArray();

        for (i = 0; i < vmdef->ndisks; i++) {
            if (virCHMonitorBuildDiskJson(disks, vmdef->disks[i]) < 0)
                goto cleanup;
        }
        if (virJSONValueObjectAppend(content, "disks", &disks) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(disks);
    return -1;
}

static int
virCHMonitorBuildNetJson(virDomainObj *vm, virJSONValue *nets, virDomainNetDef *netdef,
                         size_t *nnicindexes, int **nicindexes)
{
    virDomainNetType netType = virDomainNetGetActualType(netdef);
    char macaddr[VIR_MAC_STRING_BUFLEN];
    virCHDomainObjPrivate *priv = vm->privateData;
    virJSONValue *net;
    virJSONValue *clh_tapfds = NULL;
    int i = 0;

    net = virJSONValueNewObject();

    switch (netType) {
    case VIR_DOMAIN_NET_TYPE_ETHERNET:
        if (netdef->hostIP.nips == 1) {
            const virNetDevIPAddr *ip = netdef->hostIP.ips[0];
            g_autofree char *addr = NULL;
            virSocketAddr netmask;
            g_autofree char *netmaskStr = NULL;
            if (!(addr = virSocketAddrFormat(&ip->address)))
                goto cleanup;
            if (virJSONValueObjectAppendString(net, "ip", addr) < 0)
                goto cleanup;

            if (virSocketAddrPrefixToNetmask(ip->prefix, &netmask, AF_INET) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Failed to translate net prefix %d to netmask"),
                               ip->prefix);
                goto cleanup;
            }
            if (!(netmaskStr = virSocketAddrFormat(&netmask)))
                goto cleanup;
            if (virJSONValueObjectAppendString(net, "mask", netmaskStr) < 0)
                goto cleanup;
        } else if (netdef->guestIP.nips > 1) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("ethernet type supports a single guest ip"));
        }

        /* network and bridge use a tap device, and direct uses a
         * macvtap device
         */
        if (nicindexes && nnicindexes && netdef->ifname) {
            int nicindex;
            if (virNetDevGetIndex(netdef->ifname, &nicindex) < 0 ||
                VIR_APPEND_ELEMENT(*nicindexes, *nnicindexes, nicindex) < 0)
                goto cleanup;
        }

        break;
    case VIR_DOMAIN_NET_TYPE_VHOSTUSER:
        if ((virDomainChrType)netdef->data.vhostuser->type != VIR_DOMAIN_CHR_TYPE_UNIX) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("vhost_user type support UNIX socket in this CH"));
            goto cleanup;
        } else {
            if (virJSONValueObjectAppendString(net, "vhost_socket", netdef->data.vhostuser->data.nix.path) < 0)
                goto cleanup;
            if (virJSONValueObjectAppendBoolean(net, "vhost_user", true) < 0)
                goto cleanup;
        }
        break;
    case VIR_DOMAIN_NET_TYPE_NETWORK:
        //TAP device is created and attached to selected bridge in chProcessNetworkPrepareDevices
        //nothing more to do here
        break;
    case VIR_DOMAIN_NET_TYPE_BRIDGE:
    case VIR_DOMAIN_NET_TYPE_DIRECT:
    case VIR_DOMAIN_NET_TYPE_USER:
    case VIR_DOMAIN_NET_TYPE_SERVER:
    case VIR_DOMAIN_NET_TYPE_CLIENT:
    case VIR_DOMAIN_NET_TYPE_MCAST:
    case VIR_DOMAIN_NET_TYPE_INTERNAL:
    case VIR_DOMAIN_NET_TYPE_HOSTDEV:
    case VIR_DOMAIN_NET_TYPE_UDP:
    case VIR_DOMAIN_NET_TYPE_VDPA:
    case VIR_DOMAIN_NET_TYPE_LAST:
    default:
        virReportEnumRangeError(virDomainNetType, netType);
        goto cleanup;
    }

    clh_tapfds = virJSONValueNewArray();
    for (i=0; i< priv->tapfdSize; i++) {
        virJSONValue *tapfd = virJSONValueNewNumberUint(priv->tapfd[i]);
        if (virJSONValueArrayAppend(clh_tapfds, &tapfd) < 0) {
            virJSONValueFree(tapfd);
            goto cleanup;
        }
    }

    if (virJSONValueObjectAppend(net, "fds", &clh_tapfds) < 0)
        goto cleanup;

    virMacAddrFormat(&netdef->mac, macaddr);
    if (virJSONValueObjectAppendString(net, "mac", macaddr) < 0)
        goto cleanup;


    if (netdef->virtio != NULL) {
        if (netdef->virtio->iommu == VIR_TRISTATE_SWITCH_ON) {
            if (virJSONValueObjectAppendBoolean(net, "iommu", true) < 0)
                goto cleanup;
        }
    }
    if (priv->tapfdSize) {
        if (virJSONValueObjectAppendNumberInt(net, "num_queues", 2 * priv->tapfdSize) < 0)
            goto cleanup;
    }

    if (netdef->driver.virtio.rx_queue_size || netdef->driver.virtio.tx_queue_size) {
        if (netdef->driver.virtio.rx_queue_size != netdef->driver.virtio.tx_queue_size) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
               _("virtio rx_queue_size option %d is not same with tx_queue_size %d"),
               netdef->driver.virtio.rx_queue_size,
               netdef->driver.virtio.tx_queue_size);
            goto cleanup;
        }
        if (virJSONValueObjectAppendNumberInt(net, "queue_size", netdef->driver.virtio.rx_queue_size) < 0)
            goto cleanup;
    }

    if (virJSONValueArrayAppend(nets, &net) < 0)
        goto cleanup;

    return 0;

 cleanup:
    virJSONValueFree(net);
    virJSONValueFree(clh_tapfds);
    return -1;
}

static int
virCHMonitorBuildNetsJson(virDomainObj *vm, virJSONValue *content, virDomainDef *vmdef,
                          size_t *nnicindexes, int **nicindexes)
{
    virJSONValue *nets;
    size_t i;

    if (vmdef->nnets > 0) {
        nets = virJSONValueNewArray();

        for (i = 0; i < vmdef->nnets; i++) {
            if (virCHMonitorBuildNetJson(vm, nets, vmdef->nets[i],
                                         nnicindexes, nicindexes) < 0)
                goto cleanup;
        }
        if (virJSONValueObjectAppend(content, "net", &nets) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(nets);
    return -1;
}

static int
virCHMonitorBuildDeviceJson(virJSONValue *devices, virDomainHostdevDef *hostdevdef)
{
    virJSONValue *device;

    if (hostdevdef->mode == VIR_DOMAIN_HOSTDEV_MODE_SUBSYS &&
        hostdevdef->source.subsys.type == VIR_DOMAIN_HOSTDEV_SUBSYS_TYPE_PCI) {
        g_autofree char *name = NULL;
        g_autofree char *path = NULL;
        virDomainHostdevSubsysPCI *pcisrc = &hostdevdef->source.subsys.u.pci;
        device = virJSONValueNewObject();
        name = g_strdup_printf(VIR_PCI_DEVICE_ADDRESS_FMT, pcisrc->addr.domain,
                               pcisrc->addr.bus, pcisrc->addr.slot,
                               pcisrc->addr.function);
        path = g_strdup_printf("/sys/bus/pci/devices/%s/", name);
        if (!virFileExists(path)) {
            virReportError(VIR_ERR_DEVICE_MISSING,
                           _("host pci device %s not found"), path);
            goto cleanup;
        }
        if (virJSONValueObjectAppendString(device, "path", path) < 0)
            goto cleanup;
        if (virJSONValueArrayAppend(devices, &device) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(device);
    return -1;
}

static int
virCHMonitorBuildDevicesJson(virJSONValue *content, virDomainDef *vmdef)
{
    virJSONValue *devices;
    size_t i;

    if (vmdef->nhostdevs > 0) {
        devices = virJSONValueNewArray();
        for (i = 0; i < vmdef->nhostdevs; i++) {
            if (virCHMonitorBuildDeviceJson(devices, vmdef->hostdevs[i]) < 0)
                goto cleanup;
        }
        if (virJSONValueObjectAppend(content, "devices", &devices) < 0)
            goto cleanup;
    }

    return 0;

 cleanup:
    virJSONValueFree(devices);
    return -1;
}

static int
virCHMonitorBuildResizeCPUsJson(virJSONValue *content, unsigned int nvcpus)
{
    if (virJSONValueObjectAppendNumberUint(content, "desired_vcpus", nvcpus) < 0)
        return -1;

    return 0;
}

static int
virCHMonitorDetectUnsupportedDevices(virDomainDef *vmdef)
{
    int ret = 0;

    if (vmdef->ngraphics > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support graphics"));
        ret = 1;
    }
    if (vmdef->ncontrollers > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support controllers"));
        ret = 1;
    }
    if (vmdef->nfss > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support fss"));
        ret = 1;
    }
    if (vmdef->ninputs > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support inputs"));
        ret = 1;
    }
    if (vmdef->nsounds > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support sounds"));
        ret = 1;
    }
    if (vmdef->naudios > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support audios"));
        ret = 1;
    }
    if (vmdef->nvideos > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support videos"));
        ret = 1;
    }
    if (vmdef->nhostdevs > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support hostdevs"));
        ret = 1;
    }
    if (vmdef->nredirdevs > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support redirdevs"));
        ret = 1;
    }
    if (vmdef->nsmartcards > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support smartcards"));
        ret = 1;
    }
    if (vmdef->nserials > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support serials"));
        ret = 1;
    }
    if (vmdef->nparallels > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support parallels"));
        ret = 1;
    }
    if (vmdef->nchannels > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support channels"));
        ret = 1;
    }
    if (vmdef->nconsoles > 1) {
       virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support multiple consoles"));
        ret = 1;
    }
    if (vmdef->nleases > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support leases"));
        ret = 1;
    }
    if (vmdef->nhubs > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support hubs"));
        ret = 1;
    }
    if (vmdef->nseclabels > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support seclabels"));
        ret = 1;
    }
    if (vmdef->nrngs > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support rngs"));
        ret = 1;
    }
    if (vmdef->nshmems > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support shmems"));
        ret = 1;
    }
    if (vmdef->nmems > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support mems"));
        ret = 1;
    }
    if (vmdef->npanics > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support panics"));
        ret = 1;
    }
    if (vmdef->nsysinfo > 0) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Cloud-Hypervisor doesn't support sysinfo"));
        ret = 1;
    }

    return ret;
}

static int
virCHMonitorBuildVMJson(virDomainObj *vm, virDomainDef *vmdef, char **jsonstr,
                        size_t *nnicindexes, int **nicindexes)
{
    virJSONValue *content = virJSONValueNewObject();
    int ret = -1;

    if (vmdef == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("VM is not defined"));
        goto cleanup;
    }

    if (virCHMonitorDetectUnsupportedDevices(vmdef))
        goto cleanup;

    if (virCHMonitorBuildPTYJson(content, vmdef) < 0)
        goto cleanup;

    if (virCHMonitorBuildCPUJson(content, vmdef) < 0)
        goto cleanup;

    if (virCHMonitorBuildMemoryJson(content, vmdef) < 0)
        goto cleanup;

    if (virCHMonitorBuildKernelRelatedJson(content, vmdef) < 0)
        goto cleanup;

    if (virCHMonitorBuildDisksJson(content, vmdef) < 0)
        goto cleanup;

    if (virCHMonitorBuildNetsJson(vm, content, vmdef,
                                  nnicindexes, nicindexes) < 0)
        goto cleanup;

    if (virCHMonitorBuildDevicesJson(content, vmdef) < 0)
        goto cleanup;

    if (!(*jsonstr = virJSONValueToString(content, false)))
        goto cleanup;

    ret = 0;

 cleanup:
    virJSONValueFree(content);
    return ret;
}

static int
virCHMonitorBuildResizeJson(const unsigned int nvcpus,
                            char **jsonstr)
{
    virJSONValue *content = virJSONValueNewObject();
    int ret = -1;

    if (virCHMonitorBuildResizeCPUsJson(content, nvcpus) < 0)
        goto cleanup;

    if (!(*jsonstr = virJSONValueToString(content, false)))
        goto cleanup;

    ret = 0;

 cleanup:
    virJSONValueFree(content);
    return ret;
}

static const char *virCHMonitorEventStrings[] = {
    "vmm:starting",
    "vmm:shutdown",
    "vm:booting", "vm:booted",
    "vm:pausing", "vm:paused",
    "vm:resuming", "vm:resumed",
    "vm:snapshotting", "vm:snapshotted",
    "vm:restoring", "vm:restored",
    "vm:resizing", "vm:resized",
    "vm:shutdown", "vm:deleted",
    "cpu_manager:create_vcpu",
    "virtio-device:activated", "virtio-device:reset"
};

static virCHMonitorEvent
virCHMonitorEventFromString(const char *sourceStr,
                            const char *eventStr)
{
    int i;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;
    const char *event;

    virBufferAsprintf(&buf, "%s:%s", sourceStr, eventStr);
    event = virBufferCurrentContent(&buf);

    for (i = 0; i < virCHMonitorEventMax; i++) {
        if STREQ(event, virCHMonitorEventStrings[i])
            break;
    }

    return i;
}

static int
virCHMonitorValidateEventsJSON(virCHMonitor *mon,
                               bool *incomplete)
{
    /*
     * Marks the start of a JSON doc(starting with '{')
     */
    char *json_start = mon->buffer;
    /*
     * Marks the start of the buffer where the scan starts.
     * It could be either:
     *      - Start of the buffer. Or
     *      - Next location after a valid JSON doc.
     */
    char *scan_start = mon->buffer;
    size_t sz = mon->buf_fill_sz;
    int blocks = 0;
    int events = 0;
    int i = 0;

    if (sz == 0)
        return 0;

    /*
     * Check if the message is a wellformed JSON. Try to find all
     * wellformed JSON doc and adjust the buffer accordingly by
     * removing invalid snippets in the buffer.
     */
    do {
        if (mon->buffer[i] == '{') {
            blocks++;

            if (blocks != 1)
                continue;

            /*
             * Possible start of a valid JSON doc. Check if
             * there were any white characters or garbage
             * before the JSON doc at this location.
             */
            json_start = mon->buffer + i;
            if (scan_start != json_start) {
                int invalid_chars = json_start - scan_start;
                VIR_WARN("invalid json or white chars in buffer: %.*s",
                         invalid_chars, scan_start);
                memmove(scan_start, json_start, sz - i);
                memset(scan_start + sz - i, 0, invalid_chars);
                i -= invalid_chars;
                sz -= invalid_chars;
            }
        } else if (mon->buffer[i] == '}' && blocks != 0) {
            blocks--;
            if (blocks == 0) {
                events++;
                /*
                 * This location marks the end of a valid JSON doc.
                 * Reset the scan_start to next location.
                 */
                scan_start = mon->buffer + i + 1;
            }
        }
    } while (++i < sz);

    *incomplete = blocks != 0 ? true : false;
    mon->buf_fill_sz = sz;

    return events;
}

/*
 * Caller should have locked the Domain
 */
static inline int
virCHMonitorShutdownVm(virDomainObj *vm,
                       virDomainShutoffReason reason)
{
     virCHDriver *driver = CH_DOMAIN_PRIVATE(vm)->driver;
     g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);

     if (virCHDomainObjBeginJob(vm, CH_JOB_MODIFY))
         return -1;

     virCHProcessStop(driver, vm, reason);
     if (vm->persistent) {
       if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir))
         VIR_WARN("Failed to persist the domain after shutdown!");
     }
     virCHDomainObjEndJob(vm);

     return 0;
}

/*
 * Caller should have reference on Monitor and Domain
 */
static int
virCHMonitorProcessEvent(virCHMonitor *mon,
                         virJSONValue *eventJSON)
{
    const char *event;
    const char *source;
    virDomainObj *vm = mon->vm;
    virCHMonitorEvent ev;
    g_autofree char *timestamp = NULL;
    virCHDriver *driver = CH_DOMAIN_PRIVATE(vm)->driver;

    if (virJSONValueObjectHasKey(eventJSON, "source") == 0) {
        VIR_WARN("Invalid JSON from monitor, no source key");
        return -1;
    }
    if (virJSONValueObjectHasKey(eventJSON, "event") == 0) {
        VIR_WARN("Invalid JSON from monitor, no event key");
        return -1;
    }
    source = virJSONValueObjectGetString(eventJSON, "source");
    event = virJSONValueObjectGetString(eventJSON, "event");

    ev = virCHMonitorEventFromString(source, event);
    VIR_DEBUG("Source: %s Event: %s, ev: %d", source, event, ev);

    if ((timestamp = virTimeStringNow()) != NULL)
        chDomainLogAppendMessage(driver, vm, "%s: Source: %s Event: %s, ev: %d\n", timestamp,source, event, ev);
    switch (ev) {
        case virCHMonitorVmEventBooted:
        case virCHMonitorVmEventResumed:
        case virCHMonitorVmEventRestored:
        case virCHMonitorVirtioDeviceEventActivated:
        case virCHMonitorVirtioDeviceEventReset:
            virObjectLock(vm);
            if (virDomainObjIsActive(vm) &&
                   virCHDomainObjBeginJob(vm, CH_JOB_MODIFY) == 0) {
                virCHProcessSetupThreads(vm);
                virCHDomainObjEndJob(vm);
            }
            virObjectUnlock(vm);
            break;
        case virCHMonitorVmmEventShutdown: // shutdown inside vmm
        case virCHMonitorVmEventShutdown:
            {
                virDomainState state;

                virObjectLock(vm);
                state = virDomainObjGetState(vm, NULL);
                if ((ev == virCHMonitorVmmEventShutdown ||
                     state == VIR_DOMAIN_SHUTDOWN)) {
                    if (virCHMonitorShutdownVm(vm, VIR_DOMAIN_SHUTOFF_SHUTDOWN))
                        VIR_WARN("Failed to mark the VM(%s) as SHUTDOWN!",
                                vm->def->name);
                }
                virObjectUnlock(vm);
                break;
            }
        case virCHMonitorVmEventBooting:
        case virCHMonitorVmEventPausing:
        case virCHMonitorVmEventPaused:
        case virCHMonitorVmEventResuming:
        case virCHMonitorVmEventSnapshotting:
        case virCHMonitorVmEventSnapshotted:
        case virCHMonitorVmEventRestoring:
        case virCHMonitorVmEventResizing:
            break;
        case virCHMonitorVmEventResized:
            virObjectLock(vm);
            if (virDomainObjIsActive(vm) &&
                   virCHDomainObjBeginJob(vm, CH_JOB_MODIFY) == 0) {
                virCHProcessSetupThreads(vm);
                /* currently cloud hypervisor lazy offlines cpus so
                 * the offline cpus may still have an active thread id
                 * making virCHDomainValidateVcpuInfo fail. For now
                 * don't validate the result of resizing. */
                virCHDomainObjEndJob(vm);
            }
            virObjectUnlock(vm);
            break;
        case virCHMonitorVmEventDeleted:
        case virCHMonitorVmmEventStarting:
        case virCHMonitorCpuCreateVcpu:
            break;
        case virCHMonitorEventMax:
        default:
            VIR_WARN("unkown event from monitor!");
            break;
    }

    return 0;
}

/*
 * Helper function to find a block of valid JSON
 * from a stream of multiple JSON blocks.
 */
static inline char *end_of_json(char *str, size_t len)
{
    bool started = false;
    int blocks = 0;
    int i = 0;

    while ((i < len) && (!started || blocks > 0)) {
        if (str[i] == '{') {
            if (!started)
                started = true;
            blocks++;
        } else if (str[i] == '}') {
            blocks--;
        }
        i++;
    }

    return (i == len && blocks) ? NULL : str + i;
}

/*
 * Caller should have reference on Monitor and Domain
 */
static int virCHMonitorProcessEvents(virCHMonitor *mon, int events)
{
    ssize_t sz = mon->buf_fill_sz;
    virJSONValue *obj = NULL;
    char *buf = mon->buffer;
    int ret = 0;
    int i = 0;

    for (i = 0; i < events; i++) {
        char tmp;
        char *end = end_of_json(buf, sz);

        /*
         * end should never be NULL! We validated that there
         * is a valid JSON document before calling end_of_json,
         * and end_of_json returns NULL only if it cannot find
         * a valid JSON document.
         */
        assert(end);
        assert(end <= buf + sz);

        /*
         * We may hit a corner case where a valid JSON
         * doc happens to end right at the end of the buffer.
         * virJSONValueFromString needs '\0' end the JSON doc.
         * So we need to adjust the buffer accordingly.
         */
        if (end == mon->buffer + CH_MONITOR_BUFFER_SZ) {
            if (buf == mon->buffer) {
                /*
                 * We have a valid JSON doc same as the buffer
                 * size. As per protocol, max JSON doc should be
                 * less than the buffer size. So this is an error.
                 * Ignore this JSON doc.
                 */
                VIR_WARN("Invalid JSON doc size. Expected <= %d"
                         ", actual %lu", CH_MONITOR_BUFFER_SZ, end - buf);
                buf = end;
                sz = 0;
                break;
            }

            /*
             * Move the valid JSON doc to the start of the buffer so
             * that we can safely fit a '\0' at the end.
             * Since end == mon->buffer + CH_MONITOR_BUFFER_SZ,
             *  sz == end - buf
             */
            memmove(mon->buffer, buf, sz);
            end = mon->buffer + sz;
            buf = mon->buffer;
            *end = tmp = 0;
        } else {
            tmp = *end, *end = 0;
        }

        if ((obj = virJSONValueFromString(buf))) {
            if (virCHMonitorProcessEvent(mon, obj) < 0) {
                VIR_WARN("Failed to process the event!");
                ret = -1;
            }
            virJSONValueFree(obj);
        } else {
            VIR_WARN("Invalid JSON from monitor");
            ret = -1;
        }

        sz -= end - buf;
        assert(sz >= 0);
        *end = tmp;
        buf = end;
    }

    /*
     * If the buffer still has incomplete data, lets
     * push it to the beginning.
     */
    if (sz > 0) {
        mon->buf_offset = sz;
        memmove(mon->buffer, buf, sz);
    } else {
        mon->buf_offset = 0;
    }

    return ret;
}

static int
virCHMonitorReadProcessEvents(virCHMonitor *mon,
                              int monitor_fd)
{
    size_t max_sz = CH_MONITOR_BUFFER_SZ - mon->buf_offset;
    char *buf = mon->buffer + mon->buf_offset;
    virDomainObj *vm = mon->vm;
    virCHDomainObjPrivate *priv = vm->privateData;
    bool incomplete = false;
    int events = 0;
    size_t sz = 0;
    pid_t pid = 0;

    memset(buf, 0, max_sz);
    do {
        ssize_t ret;

        ret = read(monitor_fd, buf + sz, max_sz - sz);
        if (ret == 0 || (ret < 0 && errno == EINTR)) {
            if (virPidFileReadPathIfAlive(priv->pidfile, &pid, priv->ch_path) < 0 ||
                    (pid < 0)) {
                virObjectLock(vm);
                if (virCHMonitorShutdownVm(vm, VIR_DOMAIN_SHUTOFF_CRASHED))
                    VIR_WARN("Failed to mark the VM(%s) as SHUTDOWN!",
                             vm->def->name);
                virObjectUnlock(vm);
                return 0;
            }

            g_usleep(G_USEC_PER_SEC);
            continue;
        } else if (ret < 0) {
            /*
             * We should never reach here. read(2) says possible errors
             * are EINTR, EAGAIN, EBADF, EFAULT, EINVAL, EIO, EISDIR
             * We handle EINTR gracefully. There is some serious issue
             * if we encounter any of the other errors(either in our code
             * or in the system). Better to bail out.
             */
            VIR_ERROR("Failed to read monitor events!: %s", strerror(errno));
            abort();
        }

        sz += ret;
        mon->buf_fill_sz = sz + mon->buf_offset;
        events = virCHMonitorValidateEventsJSON(mon, &incomplete);
        VIR_DEBUG("Monitor event(size: %lu, events: %d, incomplete: %d):\n%s",
                  mon->buf_fill_sz, events, incomplete, mon->buffer);

    } while (virDomainObjIsActive(vm) && (sz < max_sz) &&
             (events == 0 || incomplete));

    /*
     * We process the events from the read buffer if
     *    - There is atleast one event in the buffer
     *    - No incomplete events in the buffer or
     *    - The buffer is full and may have incomplete entries.
     *
     * If the buffer is full, virCHMonitorProcessEvents processes
     * the completed events in the buffer and moves incomplete
     * entries to the start of the buffer and next read from the pipe
     * starts from the offset.
     */
    return (events > 0) ? virCHMonitorProcessEvents(mon, events) : events;
}

static void virCHMonitorEventLoop(void *data)
{
    virCHMonitor *mon = (virCHMonitor *)data;
    virDomainObj *vm = NULL;
    int monitor_fd;

    VIR_DEBUG("Monitor event loop thread starting");

    while((monitor_fd = open(mon->monitorpath, O_RDONLY)) < 0) {
        if (errno == EINTR) {
            g_usleep(100000); // 100 milli seconds
            continue;
        }
        /*
         * Any other error should be a BUG(kernel/libc/libvirtd)
         * (ENOMEM can happen on exceeding per-user limits)
         */
        VIR_ERROR("Failed to open the monitor FIFO(%s) read end!",
                  mon->monitorpath);
        abort();
    }
    VIR_DEBUG("Opened the monitor FIFO(%s)", mon->monitorpath);

    mon->buffer = g_malloc_n(sizeof(char), CH_MONITOR_BUFFER_SZ);
    mon->buf_offset = 0;
    mon->buf_fill_sz = 0;

    /*
     * We would need to wait until VM is initialized.
     */
    while (!(vm = virObjectRef(mon->vm)))
        g_usleep(100000);   // 100 milli seconds

    while (g_atomic_int_get(&mon->event_loop_stop) == 0) {
        VIR_DEBUG("Reading events from monitor..");
        /*
         * virCHMonitorReadProcessEvents errors out only if
         * virjson detects an invalid JSON doc and the buffer
         * in that case is automatically taken care of. We can
         * safely continue.
         */
        if (virCHMonitorReadProcessEvents(mon, monitor_fd) < 0)
            VIR_WARN("Failed to process events from monitor!");
    }
    close(monitor_fd);

    virObjectUnref(vm);
    virObjectUnref(mon);

    VIR_DEBUG("Monitor event loop thread exiting");
    return;
}

static int virCHMonitorStartEventLoop(virCHMonitor *mon)
{
    g_autofree char *name = NULL;

    name = g_strdup_printf("mon-events-%d", mon->pid);

    virObjectRef(mon);
    if (virThreadCreateFull(&mon->event_loop_thread,
                            false,
                            virCHMonitorEventLoop,
                            name,
                            false,
                            mon) < 0) {
        virObjectUnref(mon);
        return -1;

    }

    g_atomic_int_set(&mon->event_loop_stop, 0);
    return 0;
}

static void virCHMonitorStopEventLoop(virCHMonitor *mon)
{
    g_atomic_int_set(&mon->event_loop_stop, 1);
}

static int
virCHMonitorPostInitialize(virCHMonitor *mon,
                           virDomainObj *vm,
                           virCHDriver *driver)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    virCHDomainObjPrivate *priv = vm->privateData;
    int rv;

    if ((rv = virPidFileReadPath(priv->pidfile, &vm->pid)) < 0) {
        virReportSystemError(-rv,
                             _("Domain %s didn't show up"),
                             vm->def->name);
        return -1;
    }
    VIR_DEBUG("cloud-hypervisor vm=%p name=%s running with pid=%lld",
              vm, vm->def->name, (long long)vm->pid);

    mon->pid = vm->pid;

    /*
     * Opening FIFO will block until the other end is also opened.
     * This can cause a potential deadlock if done from the main
     * thread. So we defer the opening to monitor thread. The same
     * block can happen on cloud-hypervisor as well. So we start the
     * monitor thread early here and then wait in the thread until
     * cloud-hypervisor also opens the FIFO.
     */
    if (virCHMonitorStartEventLoop(mon) < 0)
        return -1;

    /* get a curl handle */
    mon->handle = curl_easy_init();

    if (virDomainObjSave(vm, driver->xmlopt, cfg->stateDir) < 0)
        return -1;

    return 0;
}

#define MONITOR_FIFO_PATH_FORMAT    "%s/%s-monitor-fifo"

virCHMonitor *
virCHMonitorOpen(virDomainObj *vm, virCHDriver *driver)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    virCHDomainObjPrivate *priv = vm->privateData;
    virCHMonitor *mon = NULL;

    /* Hold an extra reference because we can't allow 'vm' to be
     * deleted until the monitor gets its own reference. */
    virObjectRef(vm);

    if (!virDomainObjIsActive(vm)) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("domain is not running"));
        goto error;
    }

    if (virCHMonitorInitialize() < 0)
        goto error;

    if (!(mon = virObjectLockableNew(virCHMonitorClass)))
        goto error;

    mon->socketpath = g_strdup_printf("%s/%s-socket", cfg->stateDir, vm->def->name);
    /* XXX If we ever gonna change pid file pattern, come up with
     * some intelligence here to deal with old paths. */
    if (!(priv->pidfile = virPidFileBuildPath(cfg->stateDir, vm->def->name)))
        goto error;

    mon->monitorpath = g_strdup_printf(MONITOR_FIFO_PATH_FORMAT,
                                       cfg->stateDir, vm->def->name);
    if (!virFileExists(mon->monitorpath) ||
            !virFileIsNamedPipe(mon->monitorpath)) {
        VIR_ERROR("Monitor file do not exist or is not a FIFO");
        goto error;
    }

    if (virCHMonitorPostInitialize(mon, vm, driver) < 0)
        goto error;

    virObjectRef(mon);
    mon->vm = virObjectRef(vm);

 error:
    virObjectUnref(vm);
    return mon;
}

static int
chMonitorCreateSocket(const char *socket_path)
{
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to create UNIX socket"));
        goto error;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (virStrcpyStatic(addr.sun_path, socket_path) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("UNIX socket path '%s' too long"),
                       socket_path);
        goto error;
    }

    if (unlink(socket_path) < 0 && errno != ENOENT) {
        virReportSystemError(errno,
                             _("Unable to unlink %s"),
                             socket_path);
        goto error;
    }

    if (bind(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        virReportSystemError(errno,
                             _("Unable to bind to UNIX socket path '%s'"),
                             socket_path);
        goto error;
    }

    if (listen(fd, 1) < 0) {
        virReportSystemError(errno,
                             _("Unable to listen to UNIX socket path '%s'"),
                             socket_path);
        goto error;
    }

    /* We run cloud-hypervisor with umask 0002. Compensate for the umask
     * libvirtd might be running under to get the same permission
     * cloud-hypervisor would have. */
    if (virFileUpdatePerm(socket_path, 0002, 0664) < 0)
        goto error;

    return fd;

 error:
    VIR_FORCE_CLOSE(fd);
    return -1;
}

virCHMonitor *
virCHMonitorNew(virDomainObj *vm, virCHDriver *driver)
{
    virCHMonitor *ret = NULL;
    virCHMonitor *mon = NULL;
    virCommand *cmd = NULL;
    int socket_fd = 0;
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    virCHDomainObjPrivate *priv = vm->privateData;
    int i;
    int logfile = -1;
    g_autoptr(chDomainLogContext) logCtxt = NULL;

    if (virCHMonitorInitialize() < 0)
        goto cleanup;

    if (!(mon = virObjectLockableNew(virCHMonitorClass)))
        goto cleanup;

    if (!vm->def) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("VM is not defined"));
        return NULL;
    }

    /* prepare to launch Cloud-Hypervisor socket */
    mon->socketpath = g_strdup_printf("%s/%s-socket", cfg->stateDir, vm->def->name);
    if (g_mkdir_with_parents(cfg->stateDir, 0777) < 0) {
        virReportSystemError(errno,
                             _("Cannot create socket directory '%s'"),
                             cfg->stateDir);
        goto cleanup;
    }

    if (!(priv->pidfile = virPidFileBuildPath(cfg->stateDir, vm->def->name)))
        goto cleanup;

    cmd = virCommandNew(vm->def->emulator);
    virCommandSetUmask(cmd, 0x002);
    socket_fd = chMonitorCreateSocket(mon->socketpath);
    if (socket_fd < 0) {
        virReportSystemError(errno,
                             _("Cannot create socket '%s'"),
                             mon->socketpath);
        goto cleanup;
    }

    virCommandAddArg(cmd, "--api-socket");
    virCommandAddArgFormat(cmd, "fd=%d", socket_fd);
    virCommandPassFD(cmd, socket_fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);

    for (i = 0; i < priv->tapfdSize; i++) {
        virCommandPassFD(cmd, priv->tapfd[i],
                         VIR_COMMAND_PASS_FD_CLOSE_PARENT);
    }

    /* Monitor fd to listen for VM state changes */
    mon->monitorpath = g_strdup_printf(MONITOR_FIFO_PATH_FORMAT,
                                       cfg->stateDir, vm->def->name);
    if (virFileExists(mon->monitorpath) &&
        !virFileIsNamedPipe(mon->monitorpath)) {
        VIR_WARN("Monitor file (%s) is not a FIFO, trying to delete!",
                 mon->monitorpath);
        if (virFileRemove(mon->monitorpath, -1, -1) < 0) {
            VIR_ERROR("Failed to remove the file: %s", mon->monitorpath);
            goto cleanup;
        }
    }

    if (mkfifo(mon->monitorpath, S_IWUSR | S_IRUSR) < 0 &&
        errno != EEXIST) {
        virReportSystemError(errno, "%s",
                             _("Cannot create monitor FIFO"));
        goto cleanup;
    }

    virCommandAddArg(cmd, "--event-monitor");
    virCommandAddArgFormat(cmd, "path=%s", mon->monitorpath);

    if (g_mkdir_with_parents(cfg->logDir, 0777) < 0) {
        virReportSystemError(errno,
                             _("cannot create log directory %s"),
                             cfg->logDir);
        goto cleanup;
    }

    VIR_DEBUG("Creating domain log file");
    if (!(logCtxt = chDomainLogContextNew(driver, vm,
                                          CH_DOMAIN_LOG_CONTEXT_MODE_START))) {
        virLastErrorPrefixMessage("%s", _("can't connect to virtlogd"));
        goto cleanup;
    }
    logfile = chDomainLogContextGetWriteFD(logCtxt);
    /* TODO enable */
    virCommandSetOutputFD(cmd, &logfile);
    virCommandSetErrorFD(cmd, &logfile);
    virCommandSetPidFile(cmd, priv->pidfile);
    virCommandDaemonize(cmd);
    virCommandRequireHandshake(cmd);

    if (virCommandRun(cmd, NULL) != 0) {
        VIR_DEBUG("cloud-hypervisor vm=%p name=%s failed to spawn",
                  vm, vm->def->name);
        goto cleanup;
    }

    /* wait for cloud-hypervisor process to show up */
    VIR_DEBUG("Waiting for handshake from child");
    if (virCommandHandshakeWait(cmd) < 0) {
        /* Read errors from child that occurred between fork and exec. */
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Process exited prior to exec"));
        goto cleanup;
    }

    if (virCommandHandshakeNotify(cmd) < 0)
        goto cleanup;

    if (virCHMonitorPostInitialize(mon, vm, driver) < 0)
        goto cleanup;

    /* now has its own reference */
    virObjectRef(mon);
    mon->vm = virObjectRef(vm);

    ret = mon;

 cleanup:
    virCommandFree(cmd);
    return ret;
}

static void virCHMonitorDispose(void *opaque)
{
    virCHMonitor *mon = opaque;

    VIR_DEBUG("mon=%p", mon);
    virCHMonitorThreadInfoFree(mon);
    virObjectUnref(mon->vm);
}

void virCHMonitorClose(virCHMonitor *mon)
{
    if (!mon)
        return;

    if (mon->pid > 0) {
        /* try cleaning up the Cloud-Hypervisor process */
        virProcessKillPainfully(mon->pid, true);
        mon->pid = 0;
    }

    if (mon->handle)
        curl_easy_cleanup(mon->handle);

    if (mon->socketpath) {
        if (virFileExists(mon->socketpath) &&
            virFileRemove(mon->socketpath, -1, -1) < 0) {
            VIR_WARN("Unable to remove CH socket file '%s'",
                     mon->socketpath);
        }
        g_free(mon->socketpath);
    }

    if (mon->monitorpath) {
        if (virFileExists(mon->monitorpath) &&
            virFileRemove(mon->monitorpath, -1, -1) < 0) {
            VIR_WARN("Unable to remove CH monitor file '%s'",
                     mon->monitorpath);
        }
        g_free(mon->monitorpath);
    }

    virCHMonitorStopEventLoop(mon);

    virObjectUnref(mon);
    if (mon->vm)
        virObjectUnref(mon->vm);
}

static int
virCHMonitorCurlPerform(CURL *handle)
{
    CURLcode errorCode;
    long responseCode = 0;

    errorCode = curl_easy_perform(handle);

    if (errorCode != CURLE_OK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("curl_easy_perform() returned an error: %s (%d)"),
                       curl_easy_strerror(errorCode), errorCode);
        return -1;
    }

    errorCode = curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE,
                                  &responseCode);

    if (errorCode != CURLE_OK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("curl_easy_getinfo(CURLINFO_RESPONSE_CODE) returned an "
                         "error: %s (%d)"), curl_easy_strerror(errorCode),
                       errorCode);
        return -1;
    }

    if (responseCode < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("curl_easy_getinfo(CURLINFO_RESPONSE_CODE) returned a "
                         "negative response code"));
        return -1;
    }

    return responseCode;
}

int
virCHMonitorPutNoContent(virCHMonitor *mon, const char *endpoint)
{
    g_autofree char *url = NULL;
    int responseCode = 0;
    int ret = -1;
    struct curl_slist *headers = NULL;

    url = g_strdup_printf("%s/%s", URL_ROOT, endpoint);

    virObjectLock(mon);

    /* reset all options of a libcurl session handle at first */
    curl_easy_reset(mon->handle);

    headers = curl_slist_append(headers, "Expect:");
    headers = curl_slist_append(headers, "Transfer-Encoding:");

    curl_easy_setopt(mon->handle, CURLOPT_UNIX_SOCKET_PATH, mon->socketpath);
    curl_easy_setopt(mon->handle, CURLOPT_URL, url);
    curl_easy_setopt(mon->handle, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(mon->handle, CURLOPT_HTTPHEADER, headers);

    responseCode = virCHMonitorCurlPerform(mon->handle);

    virObjectUnlock(mon);

    if (responseCode == 200 || responseCode == 204)
        ret = 0;

    curl_slist_free_all(headers);

    return ret;
}

static int
virCHMonitorPut(virCHMonitor *mon,
                const char *endpoint,
                char *payload)
{
    g_autofree char *url = NULL;
    int responseCode = 0;
    int ret = -1;
    struct curl_slist *headers = NULL;

    url = g_strdup_printf("%s/%s", URL_ROOT, endpoint);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");

    virObjectLock(mon);

    /* reset all options of a libcurl session handle at first */
    curl_easy_reset(mon->handle);

    curl_easy_setopt(mon->handle, CURLOPT_UNIX_SOCKET_PATH, mon->socketpath);
    curl_easy_setopt(mon->handle, CURLOPT_URL, url);
    curl_easy_setopt(mon->handle, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(mon->handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(mon->handle, CURLOPT_POSTFIELDS, payload);

    responseCode = virCHMonitorCurlPerform(mon->handle);

    virObjectUnlock(mon);

    if (responseCode == 200 || responseCode == 204)
        ret = 0;

    curl_slist_free_all(headers);
    return ret;
}

struct curl_data {
    char *content;
    size_t size;
};

static size_t
curl_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t content_size = size * nmemb;
    struct curl_data *data = (struct curl_data *)userp;

    data->content = g_malloc0(content_size + 1);
    memcpy(data->content, contents, content_size);
    data->content[content_size] = 0;
    data->size = content_size;

    return content_size;
}

int
virCHMonitorGet(virCHMonitor *mon, const char *endpoint, virJSONValue **response)
{
    g_autofree char *url = NULL;
    int responseCode = 0;
    int ret = -1;
    struct curl_slist *headers = NULL;
    struct curl_data data;

    url = g_strdup_printf("%s/%s", URL_ROOT, endpoint);

    virObjectLock(mon);

    /* reset all options of a libcurl session handle at first */
    curl_easy_reset(mon->handle);

    curl_easy_setopt(mon->handle, CURLOPT_UNIX_SOCKET_PATH, mon->socketpath);
    curl_easy_setopt(mon->handle, CURLOPT_URL, url);

    if (response) {
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(mon->handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(mon->handle, CURLOPT_WRITEFUNCTION, curl_callback);
        curl_easy_setopt(mon->handle, CURLOPT_WRITEDATA, (void *)&data);
    }

    responseCode = virCHMonitorCurlPerform(mon->handle);

    virObjectUnlock(mon);

    if (responseCode == 200 || responseCode == 204) {
        ret = 0;
        if (response) {
            *response = virJSONValueFromString(data.content);
        }
    }

    return ret;
}

int
virCHMonitorResizeCPU(virCHMonitor *mon,
                      unsigned int nvcpus)
{
    g_autofree char *payload = NULL;
    if (virCHMonitorBuildResizeJson(nvcpus, &payload))
        return -1;

    return virCHMonitorPut(mon, URL_VM_RESIZE, payload);
}

int
virCHMonitorShutdownVMM(virCHMonitor *mon)
{
    return virCHMonitorPutNoContent(mon, URL_VMM_SHUTDOWN);
}

int
virCHMonitorCreateVM(virCHMonitor *mon,
                     size_t *nnicindexes, int **nicindexes)
{
    g_autofree char *url = NULL;
    int responseCode = 0;
    int ret = -1;
    g_autofree char *payload = NULL;
    struct curl_slist *headers = NULL;

    url = g_strdup_printf("%s/%s", URL_ROOT, URL_VM_CREATE);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (virCHMonitorBuildVMJson(mon->vm, mon->vm->def, &payload,
                                nnicindexes, nicindexes) != 0)
        return -1;

    virObjectLock(mon);

    /* reset all options of a libcurl session handle at first */
    curl_easy_reset(mon->handle);

    curl_easy_setopt(mon->handle, CURLOPT_UNIX_SOCKET_PATH, mon->socketpath);
    curl_easy_setopt(mon->handle, CURLOPT_URL, url);
    curl_easy_setopt(mon->handle, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(mon->handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(mon->handle, CURLOPT_POSTFIELDS, payload);

    responseCode = virCHMonitorCurlPerform(mon->handle);

    virObjectUnlock(mon);

    if (responseCode == 200 || responseCode == 204)
        ret = 0;

    curl_slist_free_all(headers);
    return ret;
}

int
virCHMonitorBootVM(virCHMonitor *mon)
{
    return virCHMonitorPutNoContent(mon, URL_VM_BOOT);
}

int
virCHMonitorShutdownVM(virCHMonitor *mon)
{
    return virCHMonitorPutNoContent(mon, URL_VM_SHUTDOWN);
}

int
virCHMonitorRebootVM(virCHMonitor *mon)
{
    return virCHMonitorPutNoContent(mon, URL_VM_REBOOT);
}

int
virCHMonitorSuspendVM(virCHMonitor *mon)
{
    return virCHMonitorPutNoContent(mon, URL_VM_Suspend);
}

int
virCHMonitorResumeVM(virCHMonitor *mon)
{
    return virCHMonitorPutNoContent(mon, URL_VM_RESUME);
}

static void
virCHMonitorThreadInfoFree(virCHMonitor *mon)
{
    if (mon->threads)
        g_free(mon->threads);
    mon->threads = NULL;
    mon->nthreads = 0;
}

static int
virCHMonitorTidSortOrder(const void *a, const void *b)
{
    return *(pid_t *)a - *(pid_t *)b;
}

static bool
virCHMonitorThreadInfoChanged(virCHMonitor *mon, pid_t *tids,
        size_t ntids) {
    if (!mon->nthreads || mon->nthreads != ntids)
        return true;

    for (int i = 0; i < ntids; i++) {
        if (tids[i] != mon->threads[i].tid)
            return true;
    }

    return false;
}

ssize_t
virCHMonitorRefreshThreadInfo(virCHMonitor *mon)
{
    virCHMonitorThreadInfo *info = NULL;
    g_autofree pid_t *tids = NULL;
    virDomainObj *vm;
    size_t ntids = 0;
    size_t i;

    if (!mon)
        return -1;

    vm = mon->vm;
    if (virProcessGetPids(vm->pid, &ntids, &tids) < 0) {
        virCHMonitorThreadInfoFree(mon);
        return -1;
    }

    qsort(tids, ntids, sizeof(pid_t), virCHMonitorTidSortOrder);

    if (!virCHMonitorThreadInfoChanged(mon, tids, ntids)) {
        return 0;
    }

    virCHMonitorThreadInfoFree(mon);

    info = g_new(virCHMonitorThreadInfo, ntids);

    for (i = 0; i < ntids; i++) {
        g_autofree char *proc = NULL;
        g_autofree char *data = NULL;

        info[i].tid = tids[i];

        proc = g_strdup_printf("/proc/%d/task/%d/comm",
                (int)vm->pid, (int)tids[i]);

        if (virFileReadAll(proc, (1<<16), &data) < 0) {
            info[i].type = virCHThreadTypeUnknown;
            continue;
        }

        VIR_DEBUG("VM PID: %d, TID %d, COMM: %s",
                (int)vm->pid, (int)tids[i], data);
        if (STRPREFIX(data, "vcpu")) {
            int index;
            if ((index = strtol(data + 4, NULL, 0)) < 0) {
                VIR_WARN("Index is not specified correctly");
                continue;
            }
            info[i].type = virCHThreadTypeVcpu;
            info[i].vcpuInfo.online = true;
            info[i].vcpuInfo.cpuid = index;
            VIR_DEBUG("vcpu%d -> tid: %d", index, tids[i]);
        } else if (STRPREFIX(data, "virtio")) {
            info[i].type = virCHThreadTypeIO;
            strncpy(info[i].ioInfo.thrName, data, VIRCH_THREAD_NAME_LEN - 1);
        }else {
            info[i].type = virCHThreadTypeEmulator;
            strncpy(info[i].emuInfo.thrName, data, VIRCH_THREAD_NAME_LEN - 1);
        }
    }

    mon->threads = info;
    mon->nthreads = ntids;

    return mon->nthreads;
}

/**
 * virCHMonitorGetInfo:
 * @mon: Pointer to the monitor
 * @info: Get VM info
 *
 * Retrive the VM info and store in @info
 *
 * Returns 0 on success.
 */
int
virCHMonitorGetInfo(virCHMonitor *mon, virJSONValue **info)
{
    return virCHMonitorGet(mon, URL_VM_INFO, info);
}


/**
 * virCHMonitorGetThreadInfo:
 * @mon: Pointer to the monitor
 * @refresh: Refresh thread info or not
 *
 * Retrive thread info and store to @threads
 *
 * Returns count of threads on success.
 */
size_t
virCHMonitorGetThreadInfo(virCHMonitor *mon, bool refresh,
                          virCHMonitorThreadInfo **threads)
{
    int nthreads = mon->nthreads;

    if (refresh)
        nthreads = virCHMonitorRefreshThreadInfo(mon);

    *threads = mon->threads;

    return nthreads;
}

/**
 * virCHMonitorGetIOThreads:
 * @mon: Pointer to the monitor
 * @iothreads: Location to return array of IOThreadInfo data
 *
 * Retrieve the list of iothreads defined/running for the machine
 *
 * Returns count of IOThreadInfo structures on success
 *        -1 on error.
 */
int virCHMonitorGetIOThreads(virCHMonitor *mon,
                            virDomainIOThreadInfo ***iothreads)
{
    virDomainIOThreadInfo **iothreadinfolist = NULL, *iothreadinfo = NULL;
    size_t nthreads = mon->nthreads;
    size_t niothreads=0;
    int i;

    *iothreads = NULL;
    if (nthreads == 0)
        return 0;

    iothreadinfolist = g_new(virDomainIOThreadInfo *, nthreads);

    for (i = 0; i < nthreads; i++){
        virBitmap *map = NULL;
        if (mon->threads[i].type == virCHThreadTypeIO) {
            iothreadinfo = g_new(virDomainIOThreadInfo, 1);

            iothreadinfo->iothread_id = mon->threads[i].tid;

            if (!(map = virProcessGetAffinity(iothreadinfo->iothread_id)))
                goto cleanup;

            if (virBitmapToData(map, &(iothreadinfo->cpumap),
                            &(iothreadinfo->cpumaplen)) < 0) {
                virBitmapFree(map);
                goto cleanup;
            }
            virBitmapFree(map);
            //Append to iothreadinfolist
            iothreadinfolist[niothreads] = iothreadinfo;
            niothreads++;
        }
    }

    /*
     * Shrink the array to have only niothreads elements.
     */
    if (nthreads > niothreads)
        virShrinkN(&iothreadinfolist, sizeof(virDomainIOThreadInfo *),
               &nthreads, nthreads - niothreads);

    *iothreads = iothreadinfolist;
    VIR_DEBUG("niothreads = %ld", niothreads);
    return niothreads;

    cleanup:
        if (iothreadinfolist) {
            for (i = 0; i < niothreads; i++)
                g_free(iothreadinfolist[i]);
            g_free(iothreadinfolist);
        }
        if (iothreadinfo)
            g_free(iothreadinfo);
        return -1;
}
