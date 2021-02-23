/*
 * ch_hostdev.h: Cloud Hypervisor hostdev management
 *
 * Copyright (C) 2021 Wei Liu <liuwe@microsoft.com>
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

#include "ch_conf.h"
#include "domain_conf.h"

bool
chHostdevHostSupportsPassthroughVFIO(void);

bool
chHostdevNeedsVFIO(const virDomainHostdevDef *hostdev);

int
chHostdevPrepareNVMeDisks(virCHDriverPtr driver,
                          const char *name,
                          virDomainDiskDefPtr *disks,
                          size_t ndisks);

int
chHostdevPreparePCIDevices(virCHDriverPtr driver,
                           const char *name,
                           const unsigned char *uuid,
                           virDomainHostdevDefPtr *hostdevs,
                           int nhostdevs,
                           unsigned int flags);

int
chHostdevPrepareUSBDevices(virCHDriverPtr driver,
                           const char *name,
                           virDomainHostdevDefPtr *hostdevs,
                           int nhostdevs,
                           unsigned int flags);


int
chHostdevPrepareUSBDevices(virCHDriverPtr driver,
                           const char *name,
                           virDomainHostdevDefPtr *hostdevs,
                           int nhostdevs,
                           unsigned int flags);

int
chHostdevPrepareSCSIDevices(virCHDriverPtr driver,
                            const char *name,
                            virDomainHostdevDefPtr *hostdevs,
                            int nhostdevs);

int
chHostdevPrepareSCSIVHostDevices(virCHDriverPtr driver,
                                 const char *name,
                                 virDomainHostdevDefPtr *hostdevs,
                                 int nhostdevs);

int
chHostdevPrepareMediatedDevices(virCHDriverPtr driver,
                                const char *name,
                                virDomainHostdevDefPtr *hostdevs,
                                int nhostdevs);
int
chHostdevPrepareDomainDevices(virCHDriverPtr driver,
                              virDomainDefPtr def,
                              unsigned int flags);

void
chHostdevReAttachOneNVMeDisk(virCHDriverPtr driver,
                             const char *name,
                             virStorageSourcePtr src);

void
chHostdevReAttachNVMeDisks(virCHDriverPtr driver,
                           const char *name,
                           virDomainDiskDefPtr *disks,
                           size_t ndisks);

void
chHostdevReAttachPCIDevices(virCHDriverPtr driver,
                            const char *name,
                            virDomainHostdevDefPtr *hostdevs,
                            int nhostdevs);

void
chHostdevReAttachUSBDevices(virCHDriverPtr driver,
                            const char *name,
                            virDomainHostdevDefPtr *hostdevs,
                            int nhostdevs);

void
chHostdevReAttachSCSIDevices(virCHDriverPtr driver,
                             const char *name,
                             virDomainHostdevDefPtr *hostdevs,
                             int nhostdevs);

void
chHostdevReAttachSCSIVHostDevices(virCHDriverPtr driver,
                                    const char *name,
                                    virDomainHostdevDefPtr *hostdevs,
                                    int nhostdevs);

void
chHostdevReAttachMediatedDevices(virCHDriverPtr driver,
                                 const char *name,
                                 virDomainHostdevDefPtr *hostdevs,
                                 int nhostdevs);

void
chHostdevReAttachDomainDevices(virCHDriverPtr driver,
                               virDomainDefPtr def);

int
chHostdevUpdateActivePCIDevices(virCHDriverPtr driver,
                                virDomainDefPtr def);

int
chHostdevUpdateActiveUSBDevices(virCHDriverPtr driver,
                                virDomainDefPtr def);

int
chHostdevUpdateActiveSCSIDevices(virCHDriverPtr driver,
                                 virDomainDefPtr def);

int
chHostdevUpdateActiveMediatedDevices(virCHDriverPtr driver,
                                     virDomainDefPtr def);

int
chHostdevUpdateActiveNVMeDisks(virCHDriverPtr driver,
                               virDomainDefPtr def);

int
chHostdevUpdateActiveDomainDevices(virCHDriverPtr driver,
                                   virDomainDefPtr def);
