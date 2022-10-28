/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef __NM_PTR_ARRAY_H__
#define __NM_PTR_ARRAY_H__

typedef struct _NMPtrArray {
    const gsize len;

    /* How many elements are allocated/reserved for the ptrs array.
     * Note that there is always an extra space reserved for the
     * NULL termination afterwards. It means, "len" can grow up
     * until (including) _reserved, before reallocation is necessary.
     *
     * In other words, arr->ptrs[arr->_reserved] is allocated and reserved
     * for the trailing NULL (but may be uninitialized if the array is shorter). */
    gsize _reserved;

    GDestroyNotify _destroy_fcn;

    /* This will be the NULL terminated list of pointers. If you
     * know what you are doing, you can also steal elements from
     * the list. */
    gpointer ptrs[];
} NMPtrArray;

NMPtrArray *nm_ptr_array_new(GDestroyNotify destroy_fcn, gsize reserved);

NMPtrArray *nm_ptr_array_add_n(NMPtrArray *arr, gsize n, gpointer *ptrs);

static inline NMPtrArray *
nm_ptr_array_new_n(GDestroyNotify destroy_fcn, gsize reserved, gsize n, gpointer *ptr)
{
    return nm_ptr_array_add_n(nm_ptr_array_new(destroy_fcn, NM_MAX(reserved, n)), n, ptr);
}

static inline NMPtrArray *
nm_ptr_array_new_1(GDestroyNotify destroy_fcn, gsize reserved, gpointer ptr)
{
    return nm_ptr_array_add_n(nm_ptr_array_new(destroy_fcn, reserved), 1, &ptr);
}

static inline NMPtrArray *
nm_ptr_array_add(NMPtrArray *arr, gpointer ptr)
{
    return nm_ptr_array_add_n(arr, 1, &ptr);
}

static inline NMPtrArray *
nm_ptr_array_set_free_func(NMPtrArray *arr, GDestroyNotify destroy_fcn)
{
    nm_assert(arr);

    arr->_destroy_fcn = destroy_fcn;
    return arr;
}

void nm_ptr_array_clear(NMPtrArray *arr);

void nm_ptr_array_destroy(NMPtrArray *arr);

NM_AUTO_DEFINE_FCN0(NMPtrArray *, _nm_auto_ptrarray, nm_ptr_array_destroy);
#define nm_auto_ptrarray nm_auto(_nm_auto_ptrarray)

#endif /* __NM_PTR_ARRAY_H__ */
