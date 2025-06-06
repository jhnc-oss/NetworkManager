# SPDX-License-Identifier: LGPL-2.1-or-later

mans = [
  ['NetworkManager', '8'],
  ['NetworkManager-dispatcher', '8'],
  ['NetworkManager-wait-online.service', '8'],
  ['NetworkManager.conf', '5'],
  ['nm-online', '1'],
  ['nmcli-examples', '7'],
  ['nmcli', '1'],
  ['nmtui', '1'],
  ['nm-initrd-generator', '8'],
]

if enable_ovs
  mans += [['nm-openvswitch', '7']]
endif

if enable_nm_cloud_setup
  mans += [['nm-cloud-setup', '8']]
endif

introspection_mans = [
  ['nm-settings-keyfile', '5'],
  ['nm-settings-dbus',    '5'],
  ['nm-settings-nmcli',   '5'],
]

if enable_ifcfg_rh
  introspection_mans += [['nm-settings-ifcfg-rh', '5']]
endif

built_mans = []
foreach man: mans + introspection_mans
  name = man[0] + '.' + man[1]
  if not fs.exists(name)
    built_mans = []
    break
  endif

  built_mans += name
endforeach

if enable_introspection or enable_docs
  common_ent_file = configure_file(
    input: 'common.ent.in',
    output: '@BASENAME@',
    configuration: data_conf,
  )
endif

if enable_introspection and (enable_man or enable_docs)
  xsltproc_options = [
    find_program('xsltproc'),
    '--output', '@OUTPUT@',
    '--path', meson.current_build_dir(),
    '--xinclude',
    '--nonet',
    '--stringparam', 'man.output.quietly', '1',
    '--stringparam', 'funcsynopsis.style', 'ansi',
    '--stringparam', 'man.th.extra1.suppress', '1',
    '--stringparam', 'man.authors.section.enabled', '0',
    '--stringparam', 'man.copyright.section.enabled', '0',
    '--stringparam', 'man.th.title.max.length', '30',
  ]

  docbook_xls = 'http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl'

  mans_xmls = []

  foreach man: mans
    input = man[0] + '.xml'
    content_files += join_paths(meson.current_source_dir(), input)

    output = '@0@.@1@'.format(man[0], man[1])

    # not needed if only html requested
    if enable_man
      custom_target(
        output,
        input: input,
        output: output,
        command: xsltproc_options + [docbook_xls, '@INPUT@'],
        depend_files: common_ent_file,
        install: true,
        install_dir: join_paths(nm_mandir, 'man' + man[1]),
      )
    endif
  endforeach

  merge_cmd = files(source_root / 'tools' / 'generate-docs-nm-settings-docs-merge.py')

  name = 'dbus'
  nm_settings_docs_xml_dbus = custom_target(
    'nm-settings-docs-' + name + '.xml',
    input: [merge_cmd, nm_property_infos_xml[name], nm_settings_docs_xml_gir[name]],
    output: 'nm-settings-docs-' + name + '.xml',
    command: [
      python_path,
      merge_cmd,
      '@OUTPUT@',
      nm_property_infos_xml[name],
      nm_settings_docs_xml_gir[name]
    ],
  )

  name = 'nmcli'
  nm_settings_docs_xml_nmcli = custom_target(
    'nm-settings-docs-' + name + '.xml',
    input: [merge_cmd, nm_property_infos_xml[name], gen_metadata_nm_settings_nmcli_xml, nm_settings_docs_xml_gir[name]],
    output: 'nm-settings-docs-' + name + '.xml',
    command: [
      python_path,
      merge_cmd,
      '@OUTPUT@',
      '--only-properties-from',
      gen_metadata_nm_settings_nmcli_xml,
      nm_property_infos_xml[name],
      gen_metadata_nm_settings_nmcli_xml,
      nm_settings_docs_xml_gir[name]
    ],
  )

  nm_settings_docs_xml = {
    'dbus':  nm_settings_docs_xml_dbus,
    'nmcli':  nm_settings_docs_xml_nmcli,
  }

  mans = [
    ['nm-settings-keyfile', '5', nm_property_infos_xml['keyfile']],
    ['nm-settings-dbus',    '5', nm_settings_docs_xml['dbus']],
    ['nm-settings-nmcli',   '5', nm_settings_docs_xml['nmcli']],
  ]

  if enable_ifcfg_rh
    mans += [['nm-settings-ifcfg-rh', '5', nm_property_infos_xml['ifcfg-rh']]]
  endif

  foreach man: mans
    output = man[0] + '.xml'
    template = join_paths(meson.current_source_dir(), man[0] + '.xsl')

    input = custom_target(
      output,
      input: [template, man[2]],
      output: output,
      command: xsltproc_options + ['@INPUT@'],
    )
    content_files += input

    output = '@0@.@1@'.format(man[0], man[1])

    # not needed if only html requested
    if enable_man
      custom_target(
        output,
        input: input,
        output: output,
        command: xsltproc_options + [docbook_xls, '@INPUT@'],
        install: true,
        install_dir: join_paths(nm_mandir, 'man' + man[1]),
      )
    endif
  endforeach
# not needed if only html requested
elif enable_man
  if built_mans.length() > 0
    install_man(built_mans)
  else
    error('Building manpages requires xsltproc and -Dintrospection=true, and no prebuilt manpages were found. Try building from a release tarball or using -Dman=false.')
  endif
endif
