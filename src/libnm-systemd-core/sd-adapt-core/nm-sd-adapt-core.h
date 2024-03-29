/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2014 - 2018 Red Hat, Inc.
 */

#ifndef __NM_SD_ADAPT_CORE_H__
#define __NM_SD_ADAPT_CORE_H__

#include "libnm-systemd-core/nm-default-systemd-core.h"

#include <stdbool.h>
#include <sys/resource.h>
#include <time.h>

#undef NETWORKMANAGER_COMPILATION
#include "libnm-systemd-shared/sd-adapt-shared/nm-sd-adapt-shared.h"
#undef NETWORKMANAGER_COMPILATION
#define NETWORKMANAGER_COMPILATION NM_NETWORKMANAGER_COMPILATION_SYSTEMD

#ifndef HAVE_SYS_AUXV_H
#define HAVE_SYS_AUXV_H 0
#endif

/*****************************************************************************
 * The remainder of the header is only enabled when building the systemd code
 * itself.
 *****************************************************************************/

#if (NETWORKMANAGER_COMPILATION) & NM_NETWORKMANAGER_COMPILATION_WITH_SYSTEMD

#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <elf.h>
#ifdef HAVE_SYS_AUXV_H
#include <sys/auxv.h>
#endif
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

#define HAVE_EPOLL_PWAIT2 0

/* Missing in Linux 3.2.0, in Ubuntu 12.04 */
#ifndef BPF_XOR
#define BPF_XOR 0xa0
#endif

#ifndef ETHERTYPE_LLDP
#define ETHERTYPE_LLDP 0x88cc
#endif

#ifndef HAVE_SECURE_GETENV
#ifdef HAVE___SECURE_GETENV
#define secure_getenv __secure_getenv
#else
#error neither secure_getenv nor __secure_getenv is available
#endif
#endif

#include "libnm-base/nm-base.h"

/*****************************************************************************/

#define HAVE_OPENSSL 0

static inline int
sd_notify(int unset_environment, const char *state)
{
    return 0;
}

/* Can't include both net/if.h and linux/if.h; so have to define this here */
#ifndef IF_NAMESIZE
#define IF_NAMESIZE 16
#endif

#ifndef IFNAMSIZ
#define IFNAMSIZ IF_NAMESIZE
#endif

#ifndef MAX_HANDLE_SZ
#define MAX_HANDLE_SZ 128
#endif

#include "sd-id128.h"
#include "sparse-endian.h"

#endif /* (NETWORKMANAGER_COMPILATION) & NM_NETWORKMANAGER_COMPILATION_WITH_SYSTEMD */

/*****************************************************************************/

struct sd_device;

struct sd_device *sd_device_ref(struct sd_device *self);
struct sd_device *sd_device_unref(struct sd_device *self);

#endif /* __NM_SD_ADAPT_CORE_H__ */
