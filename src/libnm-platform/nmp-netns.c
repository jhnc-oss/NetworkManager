/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2016 Red Hat, Inc.
 */

#include "libnm-glib-aux/nm-default-glib-i18n-lib.h"

#include "nmp-netns.h"

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "libnm-log-core/nm-logging.h"

/*****************************************************************************/

/* NOTE: NMPNetns and all code used here must be thread-safe! */

/* we may not call logging functions from the main-thread alone. Hence, we
 * require locking from nm-logging. Indicate that by setting NM_THREAD_SAFE_ON_MAIN_THREAD
 * to zero. */
#undef NM_THREAD_SAFE_ON_MAIN_THREAD
#define NM_THREAD_SAFE_ON_MAIN_THREAD 0

/*****************************************************************************/

#define PROC_SELF_NS_MNT "/proc/self/ns/mnt"
#define PROC_SELF_NS_NET "/proc/self/ns/net"

#define _CLONE_NS_ALL   ((int) (CLONE_NEWNS | CLONE_NEWNET))
#define _CLONE_NS_ALL_V CLONE_NEWNS, CLONE_NEWNET

static NM_UTILS_FLAGS2STR_DEFINE(_clone_ns_to_str,
                                 int,
                                 NM_UTILS_FLAGS2STR(CLONE_NEWNS, "mnt"),
                                 NM_UTILS_FLAGS2STR(CLONE_NEWNET, "net"), );

static const char *
__ns_types_to_str(int ns_types, int ns_types_already_set, char *buf, gsize len)
{
    const char *b = buf;
    char        bb[200];

    nm_strbuf_append_c(&buf, &len, '[');
    if (ns_types & ~ns_types_already_set) {
        nm_strbuf_append_str(&buf,
                             &len,
                             _clone_ns_to_str(ns_types & ~ns_types_already_set, bb, sizeof(bb)));
    }
    if (ns_types & ns_types_already_set) {
        if (ns_types & ~ns_types_already_set)
            nm_strbuf_append_c(&buf, &len, '/');
        nm_strbuf_append_str(&buf,
                             &len,
                             _clone_ns_to_str(ns_types & ns_types_already_set, bb, sizeof(bb)));
    }
    nm_strbuf_append_c(&buf, &len, ']');
    return b;
}
#define _ns_types_to_str(ns_types, ns_types_already_set, buf) \
    __ns_types_to_str(ns_types, ns_types_already_set, buf, sizeof(buf))

/*****************************************************************************/

#define _NMLOG_DOMAIN      LOGD_PLATFORM
#define _NMLOG_PREFIX_NAME "netns"
#define _NMLOG(level, netns, ...)                                               \
    G_STMT_START                                                                \
    {                                                                           \
        NMLogLevel _level = (level);                                            \
                                                                                \
        if (nm_logging_enabled(_level, _NMLOG_DOMAIN)) {                        \
            NMPNetns *_netns = (netns);                                         \
            char      _sbuf[32];                                                \
                                                                                \
            _nm_log(_level,                                                     \
                    _NMLOG_DOMAIN,                                              \
                    0,                                                          \
                    NULL,                                                       \
                    NULL,                                                       \
                    "%s%s: " _NM_UTILS_MACRO_FIRST(__VA_ARGS__),                \
                    _NMLOG_PREFIX_NAME,                                         \
                    (_netns ? nm_sprintf_buf(_sbuf,                             \
                                             "[" NM_HASH_OBFUSCATE_PTR_FMT "]", \
                                             NM_HASH_OBFUSCATE_PTR(_netns))     \
                            : "") _NM_UTILS_MACRO_REST(__VA_ARGS__));           \
        }                                                                       \
    }                                                                           \
    G_STMT_END

/*****************************************************************************/

NM_GOBJECT_PROPERTIES_DEFINE_BASE(PROP_FD_NET, PROP_FD_MNT, );

typedef struct {
    int fd_net;
    int fd_mnt;
} NMPNetnsPrivate;

struct _NMPNetns {
    GObject         parent;
    NMPNetnsPrivate _priv;
};

struct _NMPNetnsClass {
    GObjectClass parent;
};

G_DEFINE_TYPE(NMPNetns, nmp_netns, G_TYPE_OBJECT);

#define NMP_NETNS_GET_PRIVATE(self) _NM_GET_PRIVATE(self, NMPNetns, NMP_IS_NETNS)

/*****************************************************************************/

typedef struct {
    NMPNetns *netns;
    int       count;
    int       ns_types;
} NetnsInfo;

static void      _stack_push(GArray *netns_stack, NMPNetns *netns, int ns_types);
static NMPNetns *_netns_new(GError **error);

/*****************************************************************************/

static NMPNetns *
_netns_get(NetnsInfo *info)
{
    nm_assert(!info || NMP_IS_NETNS(info->netns));
    return info ? info->netns : NULL;
}

/*****************************************************************************/

static _nm_thread_local GArray *_netns_stack = NULL;

static void
_netns_stack_clear_cb(gpointer data)
{
    NetnsInfo *info = data;

    nm_assert(NMP_IS_NETNS(info->netns));
    g_object_unref(info->netns);
}

static GArray *
_netns_stack_get_impl(void)
{
    gs_unref_object NMPNetns *netns = NULL;
    gs_free_error GError     *error = NULL;
    GArray                   *s;

    s = g_array_new(FALSE, FALSE, sizeof(NetnsInfo));
    g_array_set_clear_func(s, _netns_stack_clear_cb);
    _netns_stack = s;

    nm_utils_thread_local_register_destroy(s, (GDestroyNotify) g_array_unref);

    /* at the bottom of the stack we must try to create a netns instance
     * that we never pop. It's the base to which we need to return. */
    netns = _netns_new(&error);

    if (!netns) {
        _LOGD(NULL, "failed to create initial netns: %s", error->message);
        return s;
    }

    /* we leak this instance inside the stack. */
    _stack_push(s, netns, _CLONE_NS_ALL);

    return s;
}

#define _netns_stack_get()                \
    ({                                    \
        GArray *_s = _netns_stack;        \
                                          \
        if (G_UNLIKELY(!_s))              \
            _s = _netns_stack_get_impl(); \
        _s;                               \
    })

/*****************************************************************************/

static NMPNetns *
_stack_current_netns(GArray *netns_stack, int ns_types)
{
    guint j;

    nm_assert(netns_stack && netns_stack->len > 0);

    /* we search the stack top-down to find the netns that has
     * all @ns_types set. */
    for (j = netns_stack->len; ns_types && j >= 1;) {
        NetnsInfo *info;

        info = &nm_g_array_index(netns_stack, NetnsInfo, --j);

        if (NM_FLAGS_ALL(info->ns_types, ns_types))
            return info->netns;
    }

    g_return_val_if_reached(NULL);
}

static int
_stack_current_ns_types(GArray *netns_stack, NMPNetns *netns, int ns_types)
{
    const int ns_types_check[] = {_CLONE_NS_ALL_V};
    guint     i, j;
    int       res = 0;

    nm_assert(netns);
    nm_assert(netns_stack && netns_stack->len > 0);

    /* we search the stack top-down to check which of @ns_types
     * are already set to @netns. */
    for (j = netns_stack->len; ns_types && j >= 1;) {
        NetnsInfo *info;

        info = &nm_g_array_index(netns_stack, NetnsInfo, --j);
        if (info->netns != netns) {
            ns_types = NM_FLAGS_UNSET(ns_types, info->ns_types);
            continue;
        }

        for (i = 0; i < G_N_ELEMENTS(ns_types_check); i++) {
            if (NM_FLAGS_ANY(ns_types, ns_types_check[i])
                && NM_FLAGS_ANY(info->ns_types, ns_types_check[i])) {
                res      = NM_FLAGS_SET(res, ns_types_check[i]);
                ns_types = NM_FLAGS_UNSET(ns_types, ns_types_check[i]);
            }
        }
    }

    return res;
}

static NetnsInfo *
_stack_peek(GArray *netns_stack)
{
    if (netns_stack->len > 0)
        return &nm_g_array_last(netns_stack, NetnsInfo);
    return NULL;
}

static NetnsInfo *
_stack_bottom(GArray *netns_stack)
{
    if (netns_stack->len > 0)
        return &nm_g_array_first(netns_stack, NetnsInfo);
    return NULL;
}

static void
_stack_push(GArray *netns_stack, NMPNetns *netns, int ns_types)
{
    NetnsInfo *info;

    nm_assert(netns_stack);
    nm_assert(NMP_IS_NETNS(netns));
    nm_assert(NM_FLAGS_ANY(ns_types, _CLONE_NS_ALL));
    nm_assert(!NM_FLAGS_ANY(ns_types, ~_CLONE_NS_ALL));

    info  = nm_g_array_append_new(netns_stack, NetnsInfo);
    *info = (NetnsInfo) {
        .netns    = g_object_ref(netns),
        .ns_types = ns_types,
        .count    = 1,
    };
}

static void
_stack_pop(GArray *netns_stack)
{
    NetnsInfo *info;

    nm_assert(netns_stack);
    nm_assert(netns_stack->len > 1);

    info = &nm_g_array_last(netns_stack, NetnsInfo);

    nm_assert(NMP_IS_NETNS(info->netns));
    nm_assert(info->count == 1);

    g_array_set_size(netns_stack, netns_stack->len - 1);
}

static guint
_stack_size(GArray *netns_stack)
{
    nm_assert(netns_stack);

    return netns_stack->len;
}

/*****************************************************************************/

static NMPNetns *
_netns_new(GError **error)
{
    NMPNetns *self;
    int       fd_net, fd_mnt;
    int       errsv;

    fd_net = open(PROC_SELF_NS_NET, O_RDONLY | O_CLOEXEC);
    if (fd_net == -1) {
        errsv = errno;
        g_set_error(error,
                    NM_UTILS_ERROR,
                    NM_UTILS_ERROR_UNKNOWN,
                    "Failed opening netns: %s",
                    nm_strerror_native(errsv));
        errno = errsv;
        return NULL;
    }

    fd_mnt = open(PROC_SELF_NS_MNT, O_RDONLY | O_CLOEXEC);
    if (fd_mnt == -1) {
        errsv = errno;
        g_set_error(error,
                    NM_UTILS_ERROR,
                    NM_UTILS_ERROR_UNKNOWN,
                    "Failed opening mntns: %s",
                    nm_strerror_native(errsv));
        nm_close(fd_net);
        errno = errsv;
        return NULL;
    }

    self = g_object_new(NMP_TYPE_NETNS, NMP_NETNS_FD_NET, fd_net, NMP_NETNS_FD_MNT, fd_mnt, NULL);

    _LOGD(self, "new netns (net:%d, mnt:%d)", fd_net, fd_mnt);

    return self;
}

static int
_setns(NMPNetns *self, int type)
{
    char             buf[100];
    int              fd;
    NMPNetnsPrivate *priv = NMP_NETNS_GET_PRIVATE(self);

    nm_assert(NM_IN_SET(type, _CLONE_NS_ALL_V));

    fd = (type == CLONE_NEWNET) ? priv->fd_net : priv->fd_mnt;

    _LOGt(self, "set netns(%s, %d)", _ns_types_to_str(type, 0, buf), fd);

    return setns(fd, type);
}

static gboolean
_netns_switch_push(GArray *netns_stack, NMPNetns *self, int ns_types)
{
    int errsv;

    if (NM_FLAGS_HAS(ns_types, CLONE_NEWNET)
        && !_stack_current_ns_types(netns_stack, self, CLONE_NEWNET)
        && _setns(self, CLONE_NEWNET) != 0) {
        errsv = errno;
        _LOGE(self, "failed to switch netns: %s", nm_strerror_native(errsv));
        return FALSE;
    }
    if (NM_FLAGS_HAS(ns_types, CLONE_NEWNS)
        && !_stack_current_ns_types(netns_stack, self, CLONE_NEWNS)
        && _setns(self, CLONE_NEWNS) != 0) {
        errsv = errno;
        _LOGE(self, "failed to switch mntns: %s", nm_strerror_native(errsv));

        /* try to fix the mess by returning to the previous netns. */
        if (NM_FLAGS_HAS(ns_types, CLONE_NEWNET)
            && !_stack_current_ns_types(netns_stack, self, CLONE_NEWNET)) {
            self = _stack_current_netns(netns_stack, CLONE_NEWNET);
            if (self && _setns(self, CLONE_NEWNET) != 0) {
                errsv = errno;
                _LOGE(self, "failed to restore netns: %s", nm_strerror_native(errsv));
            }
        }
        return FALSE;
    }

    return TRUE;
}

static gboolean
_netns_switch_pop(GArray *netns_stack, NMPNetns *self, int ns_types)
{
    int       errsv;
    NMPNetns *current;
    int       success = TRUE;

    if (NM_FLAGS_HAS(ns_types, CLONE_NEWNET)
        && (!self || !_stack_current_ns_types(netns_stack, self, CLONE_NEWNET))) {
        current = _stack_current_netns(netns_stack, CLONE_NEWNET);
        if (!current) {
            g_warn_if_reached();
            success = FALSE;
        } else if (_setns(current, CLONE_NEWNET) != 0) {
            errsv = errno;
            _LOGE(self, "failed to switch netns: %s", nm_strerror_native(errsv));
            success = FALSE;
        }
    }
    if (NM_FLAGS_HAS(ns_types, CLONE_NEWNS)
        && (!self || !_stack_current_ns_types(netns_stack, self, CLONE_NEWNS))) {
        current = _stack_current_netns(netns_stack, CLONE_NEWNS);
        if (!current) {
            g_warn_if_reached();
            success = FALSE;
        } else if (_setns(current, CLONE_NEWNS) != 0) {
            errsv = errno;
            _LOGE(self, "failed to switch mntns: %s", nm_strerror_native(errsv));
            success = FALSE;
        }
    }

    return success;
}

/*****************************************************************************/

int
nmp_netns_get_fd_net(NMPNetns *self)
{
    g_return_val_if_fail(NMP_IS_NETNS(self), 0);

    return NMP_NETNS_GET_PRIVATE(self)->fd_net;
}

int
nmp_netns_get_fd_mnt(NMPNetns *self)
{
    g_return_val_if_fail(NMP_IS_NETNS(self), 0);

    return NMP_NETNS_GET_PRIVATE(self)->fd_mnt;
}

/*****************************************************************************/

static gboolean
_nmp_netns_push_type(NMPNetns *self, int ns_types)
{
    GArray    *netns_stack = _netns_stack_get();
    NetnsInfo *info;
    char       sbuf[100];

    info = _stack_peek(netns_stack);
    g_return_val_if_fail(info, FALSE);

    if (info->netns == self && info->ns_types == ns_types) {
        info->count++;
        _LOGt(self,
              "push#%u* %s (increase count to %d)",
              _stack_size(netns_stack) - 1,
              _ns_types_to_str(ns_types, ns_types, sbuf),
              info->count);
        return TRUE;
    }

    _LOGD(self,
          "push#%u %s",
          _stack_size(netns_stack),
          _ns_types_to_str(ns_types, _stack_current_ns_types(netns_stack, self, ns_types), sbuf));

    if (!_netns_switch_push(netns_stack, self, ns_types))
        return FALSE;

    _stack_push(netns_stack, self, ns_types);
    return TRUE;
}

gboolean
nmp_netns_push(NMPNetns *self)
{
    g_return_val_if_fail(NMP_IS_NETNS(self), FALSE);

    return _nmp_netns_push_type(self, _CLONE_NS_ALL);
}

gboolean
nmp_netns_push_type(NMPNetns *self, int ns_types)
{
    g_return_val_if_fail(NMP_IS_NETNS(self), FALSE);
    g_return_val_if_fail(!NM_FLAGS_ANY(ns_types, ~_CLONE_NS_ALL), FALSE);

    return _nmp_netns_push_type(self, ns_types == 0 ? _CLONE_NS_ALL : ns_types);
}

NMPNetns *
nmp_netns_new(void)
{
    GArray       *netns_stack = _netns_stack_get();
    NMPNetns     *self;
    int           errsv;
    GError       *error      = NULL;
    unsigned long mountflags = 0;

    if (!_stack_peek(netns_stack)) {
        /* there are no netns instances. We cannot create a new one
         * (because after unshare we couldn't return to the original one). */
        errno = ENOTSUP;
        return NULL;
    }

    if (unshare(_CLONE_NS_ALL) != 0) {
        errsv = errno;
        _LOGE(NULL, "failed to create new net and mnt namespace: %s", nm_strerror_native(errsv));
        return NULL;
    }

    if (mount("", "/", "none", MS_SLAVE | MS_REC, NULL) != 0) {
        errsv = errno;
        _LOGE(NULL, "failed mount --make-rslave: %s", nm_strerror_native(errsv));
        goto err_out;
    }

    if (umount2("/sys", MNT_DETACH) != 0) {
        errsv = errno;
        _LOGE(NULL, "failed umount /sys: %s", nm_strerror_native(errsv));
        goto err_out;
    }

    if (access("/sys", W_OK) == -1)
        mountflags = MS_RDONLY;

    if (mount("sysfs", "/sys", "sysfs", mountflags, NULL) != 0) {
        errsv = errno;
        _LOGE(NULL, "failed mount /sys: %s", nm_strerror_native(errsv));
        goto err_out;
    }

    self = _netns_new(&error);
    if (!self) {
        errsv = errno;
        _LOGE(NULL, "failed to create netns after unshare: %s", error->message);
        g_clear_error(&error);
        goto err_out;
    }

    _stack_push(netns_stack, self, _CLONE_NS_ALL);

    return self;
err_out:
    _netns_switch_pop(netns_stack, NULL, _CLONE_NS_ALL);
    errno = errsv;
    return NULL;
}

gboolean
nmp_netns_pop(NMPNetns *self)
{
    GArray    *netns_stack = _netns_stack_get();
    NetnsInfo *info;
    int        ns_types;

    g_return_val_if_fail(NMP_IS_NETNS(self), FALSE);

    info = _stack_peek(netns_stack);

    g_return_val_if_fail(info, FALSE);
    g_return_val_if_fail(info->netns == self, FALSE);

    if (info->count > 1) {
        info->count--;
        _LOGt(self, "pop#%u* (decrease count to %d)", _stack_size(netns_stack) - 1, info->count);
        return TRUE;
    }
    g_return_val_if_fail(info->count == 1, FALSE);

    /* cannot pop the original netns. */
    g_return_val_if_fail(_stack_size(netns_stack) > 1, FALSE);

    _LOGD(self, "pop#%u", _stack_size(netns_stack) - 1);

    ns_types = info->ns_types;

    _stack_pop(netns_stack);

    return _netns_switch_pop(netns_stack, self, ns_types);
}

NMPNetns *
nmp_netns_get_current(void)
{
    return _netns_get(_stack_peek(_netns_stack_get()));
}

NMPNetns *
nmp_netns_get_initial(void)
{
    return _netns_get(_stack_bottom(_netns_stack_get()));
}

gboolean
nmp_netns_is_initial(void)
{
    GArray *netns_stack = _netns_stack_get();

    return (_netns_get(_stack_peek(netns_stack)) == _netns_get(_stack_bottom(netns_stack)));
}

/*****************************************************************************/

gboolean
nmp_netns_bind_to_path(NMPNetns *self, const char *filename, int *out_fd)
{
    gs_free char               *dirname = NULL;
    int                         errsv;
    int                         fd;
    nm_auto_pop_netns NMPNetns *netns_pop = NULL;

    g_return_val_if_fail(NMP_IS_NETNS(self), FALSE);
    g_return_val_if_fail(filename && filename[0] == '/', FALSE);

    if (!nmp_netns_push_type(self, CLONE_NEWNET))
        return FALSE;
    netns_pop = self;

    dirname = g_path_get_dirname(filename);
    if (mkdir(dirname, 0) != 0) {
        errsv = errno;
        if (errsv != EEXIST) {
            _LOGE(self,
                  "bind: failed to create directory %s: %s",
                  dirname,
                  nm_strerror_native(errsv));
            return FALSE;
        }
    }

    if ((fd = creat(filename, S_IRUSR | S_IRGRP | S_IROTH)) == -1) {
        errsv = errno;
        _LOGE(self, "bind: failed to create %s: %s", filename, nm_strerror_native(errsv));
        return FALSE;
    }
    nm_close(fd);

    if (mount(PROC_SELF_NS_NET, filename, "none", MS_BIND, NULL) != 0) {
        errsv = errno;
        _LOGE(self,
              "bind: failed to mount %s to %s: %s",
              PROC_SELF_NS_NET,
              filename,
              nm_strerror_native(errsv));
        unlink(filename);
        return FALSE;
    }

    if (out_fd) {
        if ((fd = open(filename, O_RDONLY | O_CLOEXEC)) == -1) {
            errsv = errno;
            _LOGE(self, "bind: failed to open %s: %s", filename, nm_strerror_native(errsv));
            umount2(filename, MNT_DETACH);
            unlink(filename);
            return FALSE;
        }
        *out_fd = fd;
    }

    return TRUE;
}

gboolean
nmp_netns_bind_to_path_destroy(NMPNetns *self, const char *filename)
{
    int errsv;

    g_return_val_if_fail(NMP_IS_NETNS(self), FALSE);
    g_return_val_if_fail(filename && filename[0] == '/', FALSE);

    if (umount2(filename, MNT_DETACH) != 0) {
        errsv = errno;
        _LOGE(self, "bind: failed to unmount2 %s: %s", filename, nm_strerror_native(errsv));
        return FALSE;
    }
    if (unlink(filename) != 0) {
        errsv = errno;
        _LOGE(self, "bind: failed to unlink %s: %s", filename, nm_strerror_native(errsv));
        return FALSE;
    }
    return TRUE;
}

/*****************************************************************************/

static void
set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    NMPNetns        *self = NMP_NETNS(object);
    NMPNetnsPrivate *priv = NMP_NETNS_GET_PRIVATE(self);

    switch (prop_id) {
    case PROP_FD_NET:
        /* construct-only */
        priv->fd_net = g_value_get_int(value);
        g_return_if_fail(priv->fd_net > 0);
        break;
    case PROP_FD_MNT:
        /* construct-only */
        priv->fd_mnt = g_value_get_int(value);
        g_return_if_fail(priv->fd_mnt > 0);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
nmp_netns_init(NMPNetns *self)
{}

static void
dispose(GObject *object)
{
    NMPNetns        *self = NMP_NETNS(object);
    NMPNetnsPrivate *priv = NMP_NETNS_GET_PRIVATE(self);

    nm_close(priv->fd_net);
    priv->fd_net = -1;

    nm_close(priv->fd_mnt);
    priv->fd_mnt = -1;

    G_OBJECT_CLASS(nmp_netns_parent_class)->dispose(object);
}

static void
nmp_netns_class_init(NMPNetnsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = set_property;
    object_class->dispose      = dispose;

    obj_properties[PROP_FD_NET] =
        g_param_spec_int(NMP_NETNS_FD_NET,
                         "",
                         "",
                         0,
                         G_MAXINT,
                         0,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    obj_properties[PROP_FD_MNT] =
        g_param_spec_int(NMP_NETNS_FD_MNT,
                         "",
                         "",
                         0,
                         G_MAXINT,
                         0,
                         G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, _PROPERTY_ENUMS_LAST, obj_properties);
}
