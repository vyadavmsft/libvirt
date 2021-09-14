/*
 * ch_migration.c: Cloud Hypervisor migration support
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

#include <config.h>

#include "virlog.h"
#include "virerror.h"
#include "viralloc.h"

#include "ch_migration.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_LOG_INIT("ch.ch_migration");

typedef struct _chMigrationCookie chMigrationCookie;
typedef chMigrationCookie *chMigrationCookiePtr;
struct _chMigrationCookie {
	/* Nothing for now */
};

static void
chMigrationCookieFree(chMigrationCookiePtr mig)
{
    if (!mig)
        return;

    VIR_FREE(mig);
}


static chMigrationCookiePtr
chMigrationCookieNew(virDomainObjPtr dom)
{
    chMigrationCookiePtr mig = NULL;

    if (VIR_ALLOC(mig) < 0)
        goto error;

    /* Nothing to do */
    (void)dom;

    return mig;

error:
    chMigrationCookieFree(mig);
    return NULL;
}

static int
chMigrationBakeCookie(chMigrationCookiePtr mig, char **cookieout,
                      int *cookieoutlen)
{
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!cookieout || !cookieoutlen)
        return 0;

    /* Nothing to do */
    (void)mig;

    *cookieout = NULL;
    *cookieoutlen = 0;

    VIR_DEBUG("cookielen=%d cookie=%s", *cookieoutlen, *cookieout);

    return 0;
}

static int
chMigrationEatCookie(const char *cookiein, int cookieinlen,
                     chMigrationCookiePtr *migout)
{
    chMigrationCookiePtr mig = NULL;

    if (VIR_ALLOC(mig) < 0)
        return -1;

    /* Nothing to do */
    (void)cookiein;
    (void)cookieinlen;
    *migout = mig;

    return 0;
}
