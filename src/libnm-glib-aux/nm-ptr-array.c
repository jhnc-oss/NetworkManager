/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "libnm-glib-aux/nm-default-glib-i18n-lib.h"

#include "nm-ptr-array.h"

#include "libnm-std-aux/nm-std-utils.h"

/*****************************************************************************/

static gsize
_total_size(gsize reserved)
{
    nm_assert(reserved < ((G_MAXSIZE - G_STRUCT_OFFSET(NMPtrArray, ptrs)) / sizeof(gpointer)) - 1u);

    return G_STRUCT_OFFSET(NMPtrArray, ptrs) + (sizeof(gpointer) * (reserved + 1u));
}

NMPtrArray *
nm_ptr_array_new(GDestroyNotify destroy_fcn, gsize reserved)
{
    NMPtrArray *arr;

    if (reserved < 3u)
        reserved = 3u;

    arr = g_malloc(_total_size(reserved));

    *((gsize *) &arr->len) = 0;
    arr->_reserved         = reserved;
    arr->_destroy_fcn      = destroy_fcn;
    arr->ptrs[0]           = NULL;

    return arr;
}

NMPtrArray *
nm_ptr_array_add_n(NMPtrArray *arr, gsize n, gpointer *ptrs)
{
    gsize new_reserved;
    gsize new_len;

    nm_assert(arr);

    if (n == 0)
        return arr;

    nm_assert(n < G_MAXSIZE - arr->len);
    new_len = arr->len + n;

    nm_assert(new_len > arr->len);

    /* Note that arr->_reserved does not count the element for the
     * last trailing NULL. That is, arr->ptrs[arr->_reserved] is valid.
     * In other words, new_len may be as large as arr->reserved before
     * we need to reallocate, and `arr->ptrs[new_len] = NULL` is correct. */

    if (new_len > arr->_reserved) {
        gsize n_bytes;

        /* We grow the total buffer size using nm_utils_get_next_realloc_size().
         * This quite aggressively increases the buffer size. The idea is that
         * NMPtrArray is mostly used for short lived purposes, and it's OK to
         * waste some space to reduce re-allocation. */
        n_bytes = nm_utils_get_next_realloc_size(TRUE, _total_size(new_len));

        new_reserved = n_bytes;
        new_reserved -= G_STRUCT_OFFSET(NMPtrArray, ptrs);
        new_reserved /= sizeof(gpointer);
        new_reserved--;

        nm_assert(new_len <= new_reserved);
        nm_assert(n_bytes >= _total_size(new_reserved));

        arr            = g_realloc(arr, _total_size(new_reserved));
        arr->_reserved = new_reserved;
    }

    memcpy(&arr->ptrs[arr->len], ptrs, sizeof(gpointer) * n);
    arr->ptrs[new_len]     = NULL;
    *((gsize *) &arr->len) = new_len;

    return arr;
}

void
nm_ptr_array_clear(NMPtrArray *arr)
{
    if (!arr)
        return;

    if (arr->len == 0)
        return;

    if (!arr->_destroy_fcn) {
        (*((gsize *) &arr->len)) = 0;
        arr->ptrs[0]             = NULL;
        return;
    }

    do {
        gsize    idx;
        gpointer p;

        idx = (--(*((gsize *) &arr->len)));

        p = g_steal_pointer(&arr->ptrs[idx]);

        if (p)
            arr->_destroy_fcn(p);
    } while (arr->len > 0);
}

void
nm_ptr_array_destroy(NMPtrArray *arr)
{
    if (!arr)
        return;

    nm_ptr_array_clear(arr);
    g_free(arr);
}
