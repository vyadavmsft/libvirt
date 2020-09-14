/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_domain.c: Domain manager functions for Cloud-Hypervisor driver
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

#include "ch_domain.h"
#include "datatypes.h"
#include "domain_driver.h"
#include "virfile.h"
#include "viralloc.h"
#include "virlog.h"
#include "virtime.h"
#include "virsystemd.h"
#include "virutil.h"

#include <fcntl.h>

#define VIR_FROM_THIS VIR_FROM_CH

VIR_ENUM_IMPL(virCHDomainJob,
              CH_JOB_LAST,
              "none",
              "query",
              "destroy",
              "modify",
);

VIR_LOG_INIT("ch.ch_domain");

struct _chDomainLogContext {
    GObject parent;

    int writefd;
    int readfd; /* Only used if manager == NULL */
    off_t pos;
    ino_t inode; /* Only used if manager != NULL */
    char *path;
    virLogManager *manager;
};
static void chDomainLogContextFinalize(GObject *obj);
static void ch_domain_log_context_init(chDomainLogContext *logctxt G_GNUC_UNUSED)
{
}
static void ch_domain_log_context_class_init(chDomainLogContextClass *klass)
{
    GObjectClass *obj = G_OBJECT_CLASS(klass);

    obj->finalize = chDomainLogContextFinalize;
}

G_DEFINE_TYPE(chDomainLogContext, ch_domain_log_context, G_TYPE_OBJECT);

static void
chDomainLogContextFinalize(GObject *object)
{
    chDomainLogContext *ctxt = CH_DOMAIN_LOG_CONTEXT(object);
    VIR_DEBUG("ctxt=%p", ctxt);

    virLogManagerFree(ctxt->manager);
    g_free(ctxt->path);
    VIR_FORCE_CLOSE(ctxt->writefd);
    VIR_FORCE_CLOSE(ctxt->readfd);
    G_OBJECT_CLASS(ch_domain_log_context_parent_class)->finalize(object);
}
static int
virCHDomainObjInitJob(virCHDomainObjPrivate *priv)
{
    memset(&priv->job, 0, sizeof(priv->job));

    if (virCondInit(&priv->job.cond) < 0)
        return -1;

    return 0;
}

static void
virCHDomainObjResetJob(virCHDomainObjPrivate *priv)
{
    virCHDomainJobObj *job = &priv->job;

    job->active = CH_JOB_NONE;
    job->owner = 0;
}

int
virCHDomainObjRestoreJob(virDomainObj *obj,
                         virCHDomainJobObj *job)
{
    virCHDomainObjPrivate *priv = obj->privateData;

    memset(job, 0, sizeof(*job));
    job->active = priv->job.active;
    job->owner = priv->job.owner;

    virCHDomainObjResetJob(priv);
    return 0;
}

static void
virCHDomainObjFreeJob(virCHDomainObjPrivate *priv)
{
    ignore_value(virCondDestroy(&priv->job.cond));
}

/*
 * obj must be locked before calling, virCHDriver must NOT be locked
 *
 * This must be called by anything that will change the VM state
 * in any way
 *
 * Upon successful return, the object will have its ref count increased.
 * Successful calls must be followed by EndJob eventually.
 */
int
virCHDomainObjBeginJob(virDomainObj *obj, enum virCHDomainJob job)
{
    virCHDomainObjPrivate *priv = obj->privateData;
    unsigned long long now;
    unsigned long long then;

    if (virTimeMillisNow(&now) < 0)
        return -1;
    then = now + CH_JOB_WAIT_TIME;

    while (priv->job.active) {
        VIR_DEBUG("Wait normal job condition for starting job: %s",
                  virCHDomainJobTypeToString(job));
        if (virCondWaitUntil(&priv->job.cond, &obj->parent.lock, then) < 0)
            goto error;
    }

    virCHDomainObjResetJob(priv);

    VIR_DEBUG("Starting job: %s", virCHDomainJobTypeToString(job));
    priv->job.active = job;
    priv->job.owner = virThreadSelfID();

    return 0;

 error:
    VIR_WARN("Cannot start job (%s) for domain %s;"
             " current job is (%s) owned by (%d)",
             virCHDomainJobTypeToString(job),
             obj->def->name,
             virCHDomainJobTypeToString(priv->job.active),
             priv->job.owner);

    if (errno == ETIMEDOUT)
        virReportError(VIR_ERR_OPERATION_TIMEOUT,
                       "%s", _("cannot acquire state change lock"));
    else
        virReportSystemError(errno,
                             "%s", _("cannot acquire job mutex"));
    return -1;
}

/*
 * obj must be locked and have a reference before calling
 *
 * To be called after completing the work associated with the
 * earlier virCHDomainBeginJob() call
 */
void
virCHDomainObjEndJob(virDomainObj *obj)
{
    virCHDomainObjPrivate *priv = obj->privateData;
    enum virCHDomainJob job = priv->job.active;

    VIR_DEBUG("Stopping job: %s",
              virCHDomainJobTypeToString(job));

    virCHDomainObjResetJob(priv);
    /* We indeed need to wake up ALL threads waiting because
     * grabbing a job requires checking more variables. */
    virCondBroadcast(&priv->job.cond);
}

/**
 * virCHDomainRemoveInactive:
 *
 * The caller must hold a lock to the vm.
 */
void
virCHDomainRemoveInactive(virCHDriver *driver,
                          virDomainObj *vm)
{
    if (vm->persistent) {
        /* Short-circuit, we don't want to remove a persistent domain */
        return;
    }

    virDomainObjListRemove(driver->domains, vm);
}

/**
 * virCHDomainRemoveInactiveLocked:
 *
 * The caller must hold a lock to the vm and must hold the
 * lock on driver->domains in order to call the remove obj
 * from locked list method.
 */
static void
virCHDomainRemoveInactiveLocked(virCHDriver *driver,
                                virDomainObj *vm)
{
    if (vm->persistent) {
        /* Short-circuit, we don't want to remove a persistent domain */
        return;
    }

    virDomainObjListRemoveLocked(driver->domains, vm);
}

/**
 * virCHDomainRemoveInactiveJob:
 *
 * Just like virCHDomainRemoveInactive but it tries to grab a
 * CH_JOB_MODIFY first. Even though it doesn't succeed in
 * grabbing the job the control carries with
 * virCHDomainRemoveInactive call.
 */
void
virCHDomainRemoveInactiveJob(virCHDriver *driver,
                             virDomainObj *vm)
{
    bool haveJob;

    haveJob = virCHDomainObjBeginJob(vm, CH_JOB_MODIFY) >= 0;

    virCHDomainRemoveInactive(driver, vm);

    if (haveJob)
        virCHDomainObjEndJob(vm);
}

/**
 * virCHDomainRemoveInactiveJobLocked:
 *
 * Similar to virCHomainRemoveInactiveJob,
 * except that the caller must also hold the lock @driver->domains
 */
void
virCHDomainRemoveInactiveJobLocked(virCHDriver *driver,
                                   virDomainObj *vm)
{
    bool haveJob;

    haveJob = virCHDomainObjBeginJob(vm, CH_JOB_MODIFY) >= 0;

    virCHDomainRemoveInactiveLocked(driver, vm);

    if (haveJob)
        virCHDomainObjEndJob(vm);
}

static void *
virCHDomainObjPrivateAlloc(void *opaque)
{
    virCHDomainObjPrivate *priv;

    priv = g_new0(virCHDomainObjPrivate, 1);

    if (virCHDomainObjInitJob(priv) < 0) {
        g_free(priv);
        return NULL;
    }

    if (!(priv->devs = virChrdevAlloc())) {
        g_free(priv);
        return NULL;
    }

    priv->driver = opaque;

    return priv;
}

static void
virCHDomainObjPrivateFree(void *data)
{
    virCHDomainObjPrivate *priv = data;

    virChrdevFree(priv->devs);
    virCHDomainObjFreeJob(priv);
    if (priv->pidfile)
        g_free(priv->pidfile);
    g_free(priv);
}

static int
virCHDomainDefPostParseBasic(virDomainDef *def,
                             void *opaque G_GNUC_UNUSED)
{
    /* check for emulator and create a default one if needed */
    if (!def->emulator) {
        if (!(def->emulator = g_find_program_in_path(CH_CMD))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("No emulator found for cloud-hypervisor"));
            return 1;
        }
    }

    return 0;
}

static virClass *virCHDomainVcpuPrivateClass;
static void virCHDomainVcpuPrivateDispose(void *obj);

static int
virCHDomainVcpuPrivateOnceInit(void)
{
    if (!VIR_CLASS_NEW(virCHDomainVcpuPrivate, virClassForObject()))
        return -1;

    return 0;
}

VIR_ONCE_GLOBAL_INIT(virCHDomainVcpuPrivate);

static virObject *
virCHDomainVcpuPrivateNew(void)
{
    virCHDomainVcpuPrivate *priv;

    if (virCHDomainVcpuPrivateInitialize() < 0)
        return NULL;

    if (!(priv = virObjectNew(virCHDomainVcpuPrivateClass)))
        return NULL;

    return (virObject *) priv;
}

static void
virCHDomainVcpuPrivateDispose(void *obj)
{
    virCHDomainVcpuPrivate *priv = obj;

    priv->tid = 0;

    return;
}

virDomainXMLPrivateDataCallbacks virCHDriverPrivateDataCallbacks = {
    .alloc = virCHDomainObjPrivateAlloc,
    .free = virCHDomainObjPrivateFree,
    .vcpuNew = virCHDomainVcpuPrivateNew,
};

static int
virCHDomainDefPostParse(virDomainDef *def,
                        unsigned int parseFlags G_GNUC_UNUSED,
                        void *opaque,
                        void *parseOpaque G_GNUC_UNUSED)
{
    virCHDriver *driver = opaque;
    g_autoptr(virCaps) caps = virCHDriverGetCapabilities(driver, false);
    if (!caps)
        return -1;
    if (!virCapabilitiesDomainSupported(caps, def->os.type,
                                        def->os.arch,
                                        def->virtType))
        return -1;

    return 0;
}

virDomainDefParserConfig virCHDriverDomainDefParserConfig = {
    .domainPostParseBasicCallback = virCHDomainDefPostParseBasic,
    .domainPostParseCallback = virCHDomainDefPostParse,
};

virCHMonitor *
virCHDomainGetMonitor(virDomainObj *vm)
{
    return CH_DOMAIN_PRIVATE(vm)->monitor;
}

pid_t
virCHDomainGetVcpuPid(virDomainObj *vm,
                     unsigned int vcpuid)
{
    virDomainVcpuDef *vcpu = virDomainDefGetVcpu(vm->def, vcpuid);
    return CH_DOMAIN_VCPU_PRIVATE(vcpu)->tid;
}

bool
virCHDomainHasVcpuPids(virDomainObj *vm)
{
    size_t i;
    size_t maxvcpus = virDomainDefGetVcpusMax(vm->def);
    virDomainVcpuDef *vcpu;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);

        if (CH_DOMAIN_VCPU_PRIVATE(vcpu)->tid > 0)
            return true;
    }

    return false;
}

char *virCHDomainGetMachineName(virDomainObj *vm)
{
    virCHDomainObjPrivate *priv = CH_DOMAIN_PRIVATE(vm);
    virCHDriver *driver = priv->driver;
    char *ret = NULL;

    if (vm->pid > 0) {
        ret = virSystemdGetMachineNameByPID(vm->pid);
        if (!ret)
            virResetLastError();
    }

    if (!ret)
        ret = virDomainDriverGenerateMachineName("ch",
                                                 driver->embeddedRoot,
                                                 vm->def->id, vm->def->name,
                                                 driver->privileged);

    return ret;
}

/**
 * virCHDomainValidateVcpuInfo:
 *
 * Validates vcpu thread information. If vcpu thread IDs are available,
 * this function validates that online vcpus have thread info present and
 * offline vcpus don't.
 *
 * Returns 0 on success -1 on error.
 */
int
virCHDomainValidateVcpuInfo(virDomainObj *vm)
{
    size_t maxvcpus = virDomainDefGetVcpusMax(vm->def);
    virDomainVcpuDef *vcpu;
    virCHDomainVcpuPrivate *vcpupriv;
    size_t i;

    if (!virCHDomainHasVcpuPids(vm))
        return 0;

    for (i = 0; i < maxvcpus; i++) {
        vcpu = virDomainDefGetVcpu(vm->def, i);
        vcpupriv = CH_DOMAIN_VCPU_PRIVATE(vcpu);

        if (vcpu->online && vcpupriv->tid == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Didn't find thread id for vcpu '%zu'"), i);
            return -1;
        }

        if (!vcpu->online && vcpupriv->tid != 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Found thread id for inactive vcpu '%zu'"),
                           i);
            return -1;
        }
    }

    return 0;
}

/**
 * virCHDomainObjFromDomain:
 * @domain: Domain pointer that has to be looked up
 *
 * This function looks up @domain and returns the appropriate virDomainObj *
 * that has to be released by calling virDomainObjEndAPI().
 *
 * Returns the domain object with incremented reference counter which is locked
 * on success, NULL otherwise.
 */
virDomainObj *
virCHDomainObjFromDomain(virDomain *domain)
{
    virDomainObj *vm;
    virCHDriver *driver = domain->conn->privateData;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    vm = virDomainObjListFindByUUID(driver->domains, domain->uuid);
    if (!vm) {
        virUUIDFormat(domain->uuid, uuidstr);
        virReportError(VIR_ERR_NO_DOMAIN,
                       _("no domain with matching uuid '%s' (%s)"),
                       uuidstr, domain->name);
        return NULL;
    }

    return vm;
}


chDomainLogContext *
chDomainLogContextNew(virCHDriver *driver,
                      virDomainObj *vm,
                      chDomainLogContextMode mode)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    chDomainLogContext *ctxt = CH_DOMAIN_LOG_CONTEXT(g_object_new(CH_TYPE_DOMAIN_LOG_CONTEXT, NULL));

    VIR_DEBUG("Context new %p stdioLogD=%d", ctxt, cfg->stdioLogD);
    ctxt->writefd = -1;
    ctxt->readfd = -1;

    ctxt->path = g_strdup_printf("%s/%s.log", cfg->logDir, vm->def->name);

    if (cfg->stdioLogD) {
        ctxt->manager = virLogManagerNew(driver->privileged);
        if (!ctxt->manager)
            goto error;

        ctxt->writefd = virLogManagerDomainOpenLogFile(ctxt->manager,
                                                       "ch",
                                                       vm->def->uuid,
                                                       vm->def->name,
                                                       ctxt->path,
                                                       0,
                                                       &ctxt->inode,
                                                       &ctxt->pos);
        if (ctxt->writefd < 0)
            goto error;
    } else {
        if ((ctxt->writefd = open(ctxt->path, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR)) < 0) {
            virReportSystemError(errno, _("failed to create logfile %s"),
                                 ctxt->path);
            goto error;
        }
        if (virSetCloseExec(ctxt->writefd) < 0) {
            virReportSystemError(errno, _("failed to set close-on-exec flag on %s"),
                                 ctxt->path);
            goto error;
        }

        /* For unprivileged startup we must truncate the file since
         * we can't rely on logrotate. We don't use O_TRUNC since
         * it is better for SELinux policy if we truncate afterwards */
        if (mode == CH_DOMAIN_LOG_CONTEXT_MODE_START &&
            !driver->privileged &&
            ftruncate(ctxt->writefd, 0) < 0) {
            virReportSystemError(errno, _("failed to truncate %s"),
                                 ctxt->path);
            goto error;
        }

        if (mode == CH_DOMAIN_LOG_CONTEXT_MODE_START) {
            if ((ctxt->readfd = open(ctxt->path, O_RDONLY, S_IRUSR | S_IWUSR)) < 0) {
                virReportSystemError(errno, _("failed to open logfile %s"),
                                     ctxt->path);
                goto error;
            }
            if (virSetCloseExec(ctxt->readfd) < 0) {
                virReportSystemError(errno, _("failed to set close-on-exec flag on %s"),
                                     ctxt->path);
                goto error;
            }
        }

        if ((ctxt->pos = lseek(ctxt->writefd, 0, SEEK_END)) < 0) {
            virReportSystemError(errno, _("failed to seek in log file %s"),
                                 ctxt->path);
            goto error;
        }
    }

    return ctxt;

 error:
    g_clear_object(&ctxt);
    return NULL;
}

/**
 * chDomainLogAppendMessage:
 *
 * This is a best-effort attempt to add a log message to the ch log file
 * either by using virtlogd or the legacy approach */
int
chDomainLogAppendMessage(virCHDriver *driver,
                         virDomainObj *vm,
                         const char *fmt,
                         ...)
{
    g_autoptr(virCHDriverConfig) cfg = virCHDriverGetConfig(driver);
    virLogManager *manager = NULL;
    va_list ap;
    g_autofree char *path = NULL;
    int writefd = -1;
    g_autofree char *message = NULL;
    int ret = -1;

    va_start(ap, fmt);

    message = g_strdup_vprintf(fmt, ap);

    VIR_DEBUG("Append log message (vm='%s' message='%s) stdioLogD=%d",
              vm->def->name, message, cfg->stdioLogD);

    path = g_strdup_printf("%s/%s.log", cfg->logDir, vm->def->name);

    if (cfg->stdioLogD) {
        if (!(manager = virLogManagerNew(driver->privileged)))
            goto cleanup;

        if (virLogManagerDomainAppendMessage(manager, "ch", vm->def->uuid,
                                             vm->def->name, path, message, 0) < 0)
            goto cleanup;
    } else {
        if ((writefd = open(path, O_WRONLY | O_CREAT | O_APPEND, S_IRUSR | S_IWUSR)) < 0) {
            virReportSystemError(errno, _("failed to create logfile %s"),
                                 path);
            goto cleanup;
        }

        if (safewrite(writefd, message, strlen(message)) < 0)
            goto cleanup;
    }

    ret = 0;

 cleanup:
    va_end(ap);
    VIR_FORCE_CLOSE(writefd);
    virLogManagerFree(manager);

    return ret;
}


int chDomainLogContextGetWriteFD(chDomainLogContext *ctxt)
{
    return ctxt->writefd;
}
