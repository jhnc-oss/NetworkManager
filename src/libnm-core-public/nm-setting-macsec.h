/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2017 Red Hat, Inc.
 */

#ifndef __NM_SETTING_MACSEC_H__
#define __NM_SETTING_MACSEC_H__

#if !defined(__NETWORKMANAGER_H_INSIDE__) && !defined(NETWORKMANAGER_COMPILATION)
#error "Only <NetworkManager.h> can be included directly."
#endif

#include "nm-setting.h"

G_BEGIN_DECLS

#define NM_TYPE_SETTING_MACSEC (nm_setting_macsec_get_type())
#define NM_SETTING_MACSEC(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), NM_TYPE_SETTING_MACSEC, NMSettingMacsec))
#define NM_SETTING_MACSEC_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), NM_TYPE_SETTING_MACSECCONFIG, NMSettingMacsecClass))
#define NM_IS_SETTING_MACSEC(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), NM_TYPE_SETTING_MACSEC))
#define NM_IS_SETTING_MACSEC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), NM_TYPE_SETTING_MACSEC))
#define NM_SETTING_MACSEC_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), NM_TYPE_SETTING_MACSEC, NMSettingMacsecClass))

#define NM_SETTING_MACSEC_SETTING_NAME "macsec"

#define NM_SETTING_MACSEC_PARENT        "parent"
#define NM_SETTING_MACSEC_MODE          "mode"
#define NM_SETTING_MACSEC_ENCRYPT       "encrypt"
#define NM_SETTING_MACSEC_MKA_CAK       "mka-cak"
#define NM_SETTING_MACSEC_MKA_CAK_FLAGS "mka-cak-flags"
#define NM_SETTING_MACSEC_MKA_CKN       "mka-ckn"
#define NM_SETTING_MACSEC_PORT          "port"
#define NM_SETTING_MACSEC_VALIDATION    "validation"
#define NM_SETTING_MACSEC_SEND_SCI      "send-sci"
#define NM_SETTING_MACSEC_OFFLOAD       "offload"

typedef struct _NMSettingMacsecClass NMSettingMacsecClass;

/**
 * NMSettingMacsecMode:
 * @NM_SETTING_MACSEC_MODE_PSK: The CAK is pre-shared
 * @NM_SETTING_MACSEC_MODE_EAP: The CAK is the result of participation in EAP
 *
 * #NMSettingMacsecMode controls how the CAK (Connectivity Association Key) used
 * in MKA (MACsec Key Agreement) is obtained.
 *
 * Since: 1.6
 */
typedef enum {
    NM_SETTING_MACSEC_MODE_PSK = 0,
    NM_SETTING_MACSEC_MODE_EAP = 1,
} NMSettingMacsecMode;

/**
 * NMSettingMacsecValidation:
 * @NM_SETTING_MACSEC_VALIDATION_DISABLE: All incoming frames are accepted if
 *   possible
 * @NM_SETTING_MACSEC_VALIDATION_CHECK: Non protected, invalid, or impossible to
 *   verify frames are accepted and counted as "invalid"
 * @NM_SETTING_MACSEC_VALIDATION_STRICT: Non protected, invalid, or impossible to
 *   verify frames are dropped
 *
 * #NMSettingMacsecValidation specifies a validation mode for incoming frames.
 *
 * Since: 1.6
 */
typedef enum {
    NM_SETTING_MACSEC_VALIDATION_DISABLE = 0,
    NM_SETTING_MACSEC_VALIDATION_CHECK   = 1,
    NM_SETTING_MACSEC_VALIDATION_STRICT  = 2,
} NMSettingMacsecValidation;

#define NM_SETTING_MACSEC_MKA_CAK_LENGTH 32

/* Deprecated. The CKN can be between 2 and 64 characters. */
#define NM_SETTING_MACSEC_MKA_CKN_LENGTH 64

/**
 * NMSettingMacsecOffload:
 * @NM_SETTING_MACSEC_OFFLOAD_DEFAULT: use the global default; disable if not defined
 * @NM_SETTING_MACSEC_OFFLOAD_OFF: disable offload
 * @NM_SETTING_MACSEC_OFFLOAD_PHY: request offload to the PHY
 * @NM_SETTING_MACSEC_OFFLOAD_MAC: request offload to the MAC
 *
 * These flags control the MACsec offload mode.
 *
 * Since: 1.46
 **/
typedef enum {
    NM_SETTING_MACSEC_OFFLOAD_DEFAULT = -1,
    NM_SETTING_MACSEC_OFFLOAD_OFF     = 0,
    NM_SETTING_MACSEC_OFFLOAD_PHY     = 1,
    NM_SETTING_MACSEC_OFFLOAD_MAC     = 2,
} NMSettingMacsecOffload;

NM_AVAILABLE_IN_1_6
GType nm_setting_macsec_get_type(void);
NM_AVAILABLE_IN_1_6
NMSetting *nm_setting_macsec_new(void);

NM_AVAILABLE_IN_1_6
const char *nm_setting_macsec_get_parent(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_6
NMSettingMacsecMode nm_setting_macsec_get_mode(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_6
gboolean nm_setting_macsec_get_encrypt(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_6
const char *nm_setting_macsec_get_mka_cak(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_6
NMSettingSecretFlags nm_setting_macsec_get_mka_cak_flags(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_6
const char *nm_setting_macsec_get_mka_ckn(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_6
int nm_setting_macsec_get_port(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_6
NMSettingMacsecValidation nm_setting_macsec_get_validation(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_12
gboolean nm_setting_macsec_get_send_sci(NMSettingMacsec *setting);
NM_AVAILABLE_IN_1_46
NMSettingMacsecOffload nm_setting_macsec_get_offload(NMSettingMacsec *setting);

G_END_DECLS

#endif /* __NM_SETTING_MACSEC_H__ */
