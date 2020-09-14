#include <config.h>

#include "virt-host-validate-ch.h"
#include "virt-host-validate-common.h"
#include "virarch.h"
#include "virbitmap.h"

int validateMinimumChVersion(char* ver_string)
{
	// expected ver_string example: "cloud-hypversor v0.8.1-<hash>"
    int min_major_version = 0;
    int min_minor_version = 9;
    char *ptr = strtok(ver_string, " ");
    char *last_tok = NULL;
    while(ptr != NULL)
	{
        last_tok = ptr;
		ptr = strtok(NULL, " ");
	}
    if (last_tok == NULL)
        return -1;

    // eliminate 'v'
	last_tok++;
    ptr = strtok(last_tok, ".");
	if (ptr == NULL)
		return -1;
    int majorVer = atoi(ptr); 
    ptr = strtok(NULL, ".");
	if (ptr == NULL)
		return -1;
	int minorVer = atoi(ptr);

	if (majorVer > min_major_version)
	{
		return 0;
	}
	else if (majorVer == min_major_version)
	{
		if (minorVer >= min_minor_version)
			return 0;
		else
			return -1;
	}
	else
	{
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
	const char *kvmhint = _("Check that CPU and firmware supports virtualization "
			"and kvm module is loaded");

	if (!(flags = virHostValidateGetCPUFlags()))
		return -1;

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
		case VIR_ARCH_S390:
		case VIR_ARCH_S390X:
			hasVirtFlag = true;
			if (virBitmapIsBitSet(flags, VIR_HOST_VALIDATE_CPU_FLAG_SIE))
				hasHwVirt = true;
			break;
		case VIR_ARCH_PPC64:
		case VIR_ARCH_PPC64LE:
			hasVirtFlag = true;
			hasHwVirt = true;
			break;
		default:
			hasHwVirt = false;
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

	char verResult[256];
	virHostMsgCheck("CH", "Cloud-Hypvervisor >= 0.9.0");
	if (virHostValidateCmdOutput("cloud-hypervisor --version", verResult, 256,
				VIR_HOST_VALIDATE_FAIL, "Failed to get command output of 'cloud-hypervisor --version'") < 0)
		ret = -1;
	if (validateMinimumChVersion(verResult) < 0)
	{
		virHostMsgFail(VIR_HOST_VALIDATE_FAIL,
                    _("Failed to meet minimum version of cloud-hypervisor (minimum:0.9.0)"));
		ret = -1;
	}
	else
	{
		virHostMsgPass();
	}

	return ret;
}
