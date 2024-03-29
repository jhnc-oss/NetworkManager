/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2008 - 2018 Red Hat, Inc.
 */

#ifndef __NM_GLIB_H__
#define __NM_GLIB_H__

/*****************************************************************************/

#ifndef __NM_MACROS_INTERNAL_H__
#error "nm-glib.h requires nm-macros-internal.h. Do not include this directly"
#endif

/*****************************************************************************/

#ifdef __clang__

#undef G_GNUC_BEGIN_IGNORE_DEPRECATIONS
#undef G_GNUC_END_IGNORE_DEPRECATIONS

#define G_GNUC_BEGIN_IGNORE_DEPRECATIONS \
    _Pragma("clang diagnostic push")     \
        _Pragma("clang diagnostic ignored \"-Wdeprecated-declarations\"")

#define G_GNUC_END_IGNORE_DEPRECATIONS _Pragma("clang diagnostic pop")

#endif

/*****************************************************************************/

/* glib 2.58+ defines an improved, type-safe variant of g_clear_pointer(). Reimplement
 * that.
 *
 * Note that we also have nm_clear_pointer() which is similar to g_clear_pointer()
 * and also preferred throughout. However, we do use g_clear_object(), which is
 * implemented via g_clear_pointer(). So while there are no immediate users of
 * g_clear_pointer() (use nm_clear_pointer() instead), there are indirect users
 * via g_clear_object().
 *
 * Anyway. So the glib 2.58+ version of g_clear_pointer() is only used if GLIB_VERSION_MAX_ALLOWED
 * is set, which we don't do.
 *
 * To get the type-safe variant, reimplement g_clear_pointer() below. This aims to
 * be identical to the version of the macro in glib 2.58.
 *
 * Still, don't use g_clear_pointer() directly (use nm_clear_pointer()). This compat
 * implementation exists only for g_clear_object().
 */
#undef g_clear_pointer

#define g_clear_pointer(pp, destroy)                       \
    G_STMT_START                                           \
    {                                                      \
        typeof((pp)) _pp  = (pp);                          \
        typeof(*_pp) _ptr = *_pp;                          \
                                                           \
        G_STATIC_ASSERT(sizeof *(pp) == sizeof(gpointer)); \
                                                           \
        *_pp = NULL;                                       \
        if (_ptr)                                          \
            (destroy)(_ptr);                               \
    }                                                      \
    G_STMT_END

/*****************************************************************************/

/* g_assert_cmpmem() is only available since glib 2.46. */
#if !GLIB_CHECK_VERSION(2, 45, 7)
#define g_assert_cmpmem(m1, l1, m2, l2)                                                 \
    G_STMT_START                                                                        \
    {                                                                                   \
        gconstpointer __m1 = m1, __m2 = m2;                                             \
        int           __l1 = l1, __l2 = l2;                                             \
        if (__l1 != __l2)                                                               \
            g_assertion_message_cmpnum(G_LOG_DOMAIN,                                    \
                                       __FILE__,                                        \
                                       __LINE__,                                        \
                                       G_STRFUNC,                                       \
                                       #l1 " (len(" #m1 ")) == " #l2 " (len(" #m2 "))", \
                                       __l1,                                            \
                                       "==",                                            \
                                       __l2,                                            \
                                       'i');                                            \
        else if (memcmp(__m1, __m2, __l1) != 0)                                         \
            g_assertion_message(G_LOG_DOMAIN,                                           \
                                __FILE__,                                               \
                                __LINE__,                                               \
                                G_STRFUNC,                                              \
                                "assertion failed (" #m1 " == " #m2 ")");               \
    }                                                                                   \
    G_STMT_END
#endif

/*****************************************************************************/

/* Rumtime check for glib version. First do a compile time check which
 * (if satisfied) shortcuts the runtime check. */
static inline gboolean
nm_glib_check_version(guint major, guint minor, guint micro)
{
    return GLIB_CHECK_VERSION(major, minor, micro)
           || ((glib_major_version > major)
               || (glib_major_version == major && glib_minor_version > minor)
               || (glib_major_version == major && glib_minor_version == minor
                   && glib_micro_version < micro));
}

/*****************************************************************************/

#if !GLIB_CHECK_VERSION(2, 54, 0)
static inline gboolean
g_ptr_array_find(GPtrArray *haystack, gconstpointer needle, guint *index_)
{
    guint i;
    g_return_val_if_fail(haystack, FALSE);

    for (i = 0; i < haystack->len; i++) {
        if (haystack->pdata[i] == needle) {
            if (index_)
                *index_ = i;
            return TRUE;
        }
    }
    return FALSE;
}
#else
#define g_ptr_array_find(haystack, needle, index_)  \
    ({                                              \
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS            \
        g_ptr_array_find(haystack, needle, index_); \
        G_GNUC_END_IGNORE_DEPRECATIONS              \
    })
#endif

/*****************************************************************************/

#ifdef g_steal_pointer
#undef g_steal_pointer
#endif

#define g_steal_pointer(pp)              \
    ({                                   \
        typeof(*(pp)) *const _pp = (pp); \
        typeof(*_pp)         _p  = *_pp; \
                                         \
        _NM_ENSURE_POINTER(_p);          \
                                         \
        *_pp = NULL;                     \
        _p;                              \
    })

/*****************************************************************************/

static inline gboolean
_nm_g_strv_contains(const char *const *strv, const char *str)
{
#if !GLIB_CHECK_VERSION(2, 44, 0)
    g_return_val_if_fail(strv != NULL, FALSE);
    g_return_val_if_fail(str != NULL, FALSE);

    for (; *strv != NULL; strv++) {
        if (g_str_equal(str, *strv))
            return TRUE;
    }

    return FALSE;
#else
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    return g_strv_contains(strv, str);
    G_GNUC_END_IGNORE_DEPRECATIONS
#endif
}
#define g_strv_contains _nm_g_strv_contains

/*****************************************************************************/

/* Recent glib also casts the results to typeof(Obj), but only if
 *
 *  ( defined(g_has_typeof) && GLIB_VERSION_MAX_ALLOWED >= GLIB_VERSION_2_56 )
 *
 * Since we build NetworkManager with older GLIB_VERSION_MAX_ALLOWED, it's
 * not taking effect.
 *
 * Override this. */
#undef g_object_ref
#undef g_object_ref_sink
#define g_object_ref(Obj)      ((typeof(Obj)) g_object_ref(Obj))
#define g_object_ref_sink(Obj) ((typeof(Obj)) g_object_ref_sink(Obj))

/*****************************************************************************/

#ifndef g_autofree
/* we still don't rely on recent glib to provide g_autofree. Hence, we continue
 * to use our gs_* free macros that we took from libgsystem.
 *
 * To ease migration towards g_auto*, add a compat define for g_autofree. */
#define g_autofree gs_free
#endif

/*****************************************************************************/

#if !GLIB_CHECK_VERSION(2, 47, 1)
/* Older versions of g_value_unset() only allowed to unset a GValue which
 * was initialized previously. This was relaxed ([1], [2], [3]).
 *
 * Our nm_auto_unset_gvalue macro requires to be able to call g_value_unset().
 * Also, it is our general practice to allow for that. Add a compat implementation.
 *
 * [1] https://gitlab.gnome.org/GNOME/glib/commit/4b2d92a864f1505f1b08eb639d74293fa32681da
 * [2] commit "Allow passing unset GValues to g_value_unset()"
 * [3] https://bugzilla.gnome.org/show_bug.cgi?id=755766
 */
static inline void
_nm_g_value_unset(GValue *value)
{
    g_return_if_fail(value);

    if (value->g_type != 0)
        g_value_unset(value);
}
#define g_value_unset _nm_g_value_unset
#endif

/* G_PID_FORMAT was added only in 2.53.5. Define it ourself.
 *
 * If this was about "pid_t", we would check SIZEOF_PID_T, and set
 * PRIi32/PRIi16, like systemd does. But it's actually about
 * GPid, which glib typedefs as an "int".
 *
 * There is a test_gpid() that check that GPid is really a typedef
 * for int. */
#undef G_PID_FORMAT
#define G_PID_FORMAT "i"

/*****************************************************************************/

/* G_SOURCE_FUNC was added in 2.57.2. */
#undef G_SOURCE_FUNC
#define G_SOURCE_FUNC(f) ((GSourceFunc) (void (*)(void))(f))

/*****************************************************************************/

/* g_atomic_pointer_get() is implemented as a macro, and it is also used for
 * (gsize *) arguments. However, that leads to compiler warnings in certain
 * configurations. Work around it, by redefining the macro. */
static inline gpointer
_g_atomic_pointer_get(void **atomic)
{
    return g_atomic_pointer_get(atomic);
}
#undef g_atomic_pointer_get
#define g_atomic_pointer_get(atomic)                                           \
    ({                                                                         \
        typeof(*atomic) *const _atomic = (atomic);                             \
                                                                               \
        /* g_atomic_pointer_get() is used by glib also for (gsize *) pointers,
         * not only pointers to pointers. We thus don't enforce that (*atomic)
         * is a pointer, but of suitable size/alignment. */ \
                                                                               \
        G_STATIC_ASSERT(sizeof(*_atomic) == sizeof(gpointer));                 \
        G_STATIC_ASSERT(_nm_alignof(*_atomic) == _nm_alignof(gpointer));       \
        (void) (0 ? (gpointer) * (_atomic) : NULL);                            \
                                                                               \
        (typeof(*_atomic)) _g_atomic_pointer_get((void **) _atomic);           \
    })

/* Reimplement g_atomic_pointer_set() macro too. Our variant does more type
 * checks. */
static inline void
_g_atomic_pointer_set(void **atomic, void *newval)
{
    return g_atomic_pointer_set(atomic, newval);
}
#undef g_atomic_pointer_set
#define g_atomic_pointer_set(atomic, newval)                        \
    ({                                                              \
        typeof(*atomic) *const         _atomic         = (atomic);  \
        typeof(*_atomic) const         _newval         = (newval);  \
        _nm_unused gconstpointer const _val_type_check = _newval;   \
                                                                    \
        (void) (0 ? (gpointer) * (_atomic) : NULL);                 \
                                                                    \
        _g_atomic_pointer_set((void **) _atomic, (void *) _newval); \
    })

/* Glib implements g_atomic_pointer_compare_and_exchange() as a macro.
 * For one, to inline the atomic operation and also to perform some type checks
 * on the arguments.
 * Depending on compiler and glib version, glib passes the arguments as they
 * are to __atomic_compare_exchange_n(). Some clang version don't accept const
 * pointers there. Reimplement the macro to get that right, but with stronger
 * type checks (as we use typeof()). Had one job. */
static inline gboolean
_g_atomic_pointer_compare_and_exchange(void **atomic, void *oldval, void *newval)
{
    return g_atomic_pointer_compare_and_exchange(atomic, oldval, newval);
}
#undef g_atomic_pointer_compare_and_exchange
#define g_atomic_pointer_compare_and_exchange(atomic, oldval, newval) \
    ({                                                                \
        typeof(*atomic) *const         _atomic         = (atomic);    \
        typeof(*_atomic) const         _oldval         = (oldval);    \
        typeof(*_atomic) const         _newval         = (newval);    \
        _nm_unused gconstpointer const _val_type_check = _oldval;     \
                                                                      \
        (void) (0 ? (gpointer) * (_atomic) : NULL);                   \
                                                                      \
        _g_atomic_pointer_compare_and_exchange((void **) _atomic,     \
                                               (void *) _oldval,      \
                                               (void *) _newval);     \
    })

/*****************************************************************************/

#if !GLIB_CHECK_VERSION(2, 58, 0)
static inline gboolean
g_hash_table_steal_extended(GHashTable   *hash_table,
                            gconstpointer lookup_key,
                            gpointer     *stolen_key,
                            gpointer     *stolen_value)
{
    g_assert(stolen_key);
    g_assert(stolen_value);

    if (g_hash_table_lookup_extended(hash_table, lookup_key, stolen_key, stolen_value)) {
        g_hash_table_steal(hash_table, lookup_key);
        return TRUE;
    }
    *stolen_key   = NULL;
    *stolen_value = NULL;
    return FALSE;
}
#else
#define g_hash_table_steal_extended(hash_table, lookup_key, stolen_key, stolen_value)    \
    ({                                                                                   \
        gpointer *_stolen_key   = (stolen_key);                                          \
        gpointer *_stolen_value = (stolen_value);                                        \
                                                                                         \
        /* we cannot allow NULL arguments, because then we would leak the values in
             * the compat implementation. */      \
        g_assert(_stolen_key);                                                           \
        g_assert(_stolen_value);                                                         \
                                                                                         \
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS                                                 \
        g_hash_table_steal_extended(hash_table, lookup_key, _stolen_key, _stolen_value); \
        G_GNUC_END_IGNORE_DEPRECATIONS                                                   \
    })
#endif

/*****************************************************************************/

_nm_deprecated("Don't use this API") void _nm_forbidden_glib_api_0(void);
_nm_deprecated("Don't use this API") void _nm_forbidden_glib_api_n(gconstpointer arg0, ...);

#undef g_cancellable_reset
#define g_cancellable_reset(cancellable) _nm_forbidden_glib_api_n(cancellable)

#undef g_idle_remove_by_data
#define g_idle_remove_by_data(data) _nm_forbidden_glib_api_n(data)

#undef g_source_remove_by_funcs_user_data
#define g_source_remove_by_funcs_user_data(funcs, user_data) \
    _nm_forbidden_glib_api_n(funcs, user_data)

#undef g_source_remove_by_user_data
#define g_source_remove_by_user_data(user_data) _nm_forbidden_glib_api_n(user_data)

/*****************************************************************************/

/* Coverity gets confused by g_strstrip(g_strdup(foo)). Reimplement the macro
 * in a way that hopefully works better to avoid the false positive. */
#undef g_strstrip
#define g_strstrip(str)           \
    ({                            \
        char *const _str = (str); \
                                  \
        g_strchug(_str);          \
        g_strchomp(_str);         \
        _str;                     \
    })

/*****************************************************************************/

/* g_alloca0() evaluates the "size" argument multiple times. That seems an error
 * prone API (as it's not function-like).
 *
 * We could fix it by using an expression statement. But it doesn't seem
 * worth it, so hide it to prevent its use. */
#undef g_alloca0
#undef g_newa0

/*****************************************************************************/

/* Use either NM_MIN()/NM_MAX() or (if that doesn't work) use NM_MIN_CONST()/NM_MAX_CONST(). */
#undef MIN
#undef MAX

/*****************************************************************************/

#endif /* __NM_GLIB_H__ */
