# SPDX-License-Identifier: LGPL-2.1-or-later

configure_file(
  input: 'server.conf.in',
  output: '@BASENAME@',
  install_dir: join_paths(nm_datadir, 'doc', nm_name, 'examples'),
  configuration: data_conf,
)

if install_systemdunitdir
  services = [
    'NetworkManager-dispatcher.service.in',
    'NetworkManager.service.in',
    'nm-priv-helper.service.in',
    'NetworkManager-wait-online.service.in',
  ]

  foreach service: services
    configure_file(
      input: service,
      output: '@BASENAME@',
      install_dir: systemd_systemdsystemunitdir,
      configuration: data_conf,
    )
  endforeach

  if enable_ovs
    install_data(
      'NetworkManager-ovs.conf',
      install_dir: join_paths(systemd_systemdsystemunitdir, 'NetworkManager.service.d'),
    )
  endif
endif

if install_udevdir
  data = files(
    '84-nm-drivers.rules',
    '85-nm-unmanaged.rules',
    '90-nm-thunderbolt.rules',
  )

  install_data(
    data,
    install_dir: join_paths(udev_udevdir, 'rules.d'),
  )
endif

if enable_polkit
  policy = 'org.freedesktop.NetworkManager.policy'

  policy_in = configure_file(
    input: policy + '.in.in',
    output: '@BASENAME@',
    configuration: data_conf,
  )

  i18n.merge_file(
    input: policy_in,
    output: '@BASENAME@',
    po_dir: po_dir,
    install: true,
    install_dir: polkit_gobject_policydir,
  )
endif

if enable_firewalld_zone
  install_data(
    'nm-shared.xml',
    install_dir: join_paths(nm_prefix, 'lib', 'firewalld', 'zones')
  )
endif
