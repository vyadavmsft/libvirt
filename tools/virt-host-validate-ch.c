#include <config.h>

#include "virt-host-validate-ch.h"
#include "virt-host-validate-common.h"
#include "virarch.h"
#include "virbitmap.h"
#include "vircommand.h"
#include "virstring.h"

int validateMinimumChVersion(char* ver_string)
{
    // expected ver_string example: "cloud-hypversor v0.8.1-<hash>"
    const int min_major_version = 0;
    const int min_minor_version = 10;
    int majorVer = 0;
    int minorVer = 0;
    char *ptr = NULL;
    char *savedptr = NULL;
    char *last_tok = NULL;

    ptr = strtok_r(ver_string, " ", &savedptr);
    while (ptr != NULL) {
        last_tok = ptr;
        ptr = strtok_r(NULL, " ", &savedptr);
    }
    if (last_tok == NULL)
        return -1;

    // eliminate 'v'
    last_tok++;
    ptr = strtok_r(last_tok, ".", &savedptr);
    if (ptr == NULL)
        return -1;
    if (virStrToLong_i(ptr, NULL, 10, &majorVer) < 0)
        return -1;
    ptr = strtok_r(NULL, ".", &savedptr);
    if (ptr == NULL)
        return -1;
    if (virStrToLong_i(ptr, NULL, 10, &minorVer) < 0)
        return -1;

    if (majorVer > min_major_version) {
        return 0;
    } else if (majorVer == min_major_version) {
        if (minorVer >= min_minor_version)
            return 0;
        else
            return -1;
    } else {
        return -1;
    }
}

int virHostValidateCh(void)
{
    int ret = 0;
    virBitmapPtr flags;
    bool hasHwVirt = false;
    bool hasVirtFlag = false;
    virArch arch = virArchFromHost();
    const char *kvmhint = _("Check that CPU and firmware supports CH virtualization "
            "and kvm module is loaded");
    virCommandPtr cmd = NULL;
    g_autofree char *outbuf = NULL;
    int exitstatus = 0;

    if (!(flags = virHostValidateGetCPUFlags()))
        return -1;

    // Unlick QEMU, Cloud-Hypervisor only supports x86_64 and aarch64
    switch ((int)arch) {
        case VIR_ARCH_I686:
        case VIR_ARCH_X86_64:
            hasVirtFlag = true;
            kvmhint = _("Check that the 'kvm-intel' or 'kvm-amd' modules are "
                    "loaded & the BIOS has enabled virtualization");
            if (virBitmapIsBitSet(flags, VIR_HOST_VALIDATE_CPU_FLAG_SVM) ||
                    virBitmapIsBitSet(flags, VIR_HOST_VALIDATE_CPU_FLAG_VMX))
                hasHwVirt = true;
            break;
        case VIR_ARCH_AARCH64:
            hasVirtFlag = true;
            hasHwVirt = true;
            break;
        default:
            hasHwVirt = false;
            break;
    }

    if (hasVirtFlag) {
        virHostMsgCheck("CH", "%s", _("for hardware virtualization"));
        if (hasHwVirt) {
            virHostMsgPass();
        } else {
            virHostMsgFail(VIR_HOST_VALIDATE_FAIL,
                    _("Only emulated CPUs are available, performance will be significantly limited"));
            ret = -1;
        }
    }

    if (hasHwVirt || !hasVirtFlag) {
        if (virHostValidateDeviceExists("CH", "/dev/kvm",
                    VIR_HOST_VALIDATE_FAIL,
                    kvmhint) <0)
            ret = -1;
        else if (virHostValidateDeviceAccessible("CH", "/dev/kvm",
                    VIR_HOST_VALIDATE_FAIL,
                    _("Check /dev/kvm is world writable or you are in "
                        "a group that is allowed to access it")) < 0)
            ret = -1;
    }

    virHostMsgCheck("CH", "Cloud-Hypvervisor >= 0.10.0");
    cmd = virCommandNewArgList("cloud-hypervisor", "--version", NULL);
    virCommandSetOutputBuffer(cmd, &outbuf);
    ret = virCommandRun(cmd, &exitstatus);
    if (ret < 0) {
        virHostMsgFail(VIR_HOST_VALIDATE_FAIL,
                _("Failed to check cloud-hypervisor version via `cloud-hypervisor --version`"));
    }
    if (!outbuf) {
        // no output
        virHostMsgFail(VIR_HOST_VALIDATE_FAIL,
                _("no output: `cloud-hypervisor --version`"));
        ret = -1;
    }
    if (validateMinimumChVersion(outbuf) < 0) {
        virHostMsgFail(VIR_HOST_VALIDATE_FAIL,
                _("Failed to meet minimum version of cloud-hypervisor (minimum:0.10.0)"));
        ret = -1;
    } else {
        virHostMsgPass();
    }

    return ret;
}
