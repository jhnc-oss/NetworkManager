# SPDX-License-Identifier: LGPL-2.1-or-later

if enable_introspection
  xsltproc = find_program('xsltproc')

  settings = 'settings-spec'
  output = settings + '.xml'

  inputs = [
    settings + '.xsl',
    nm_settings_docs_xml['dbus'],
  ]

  content_files += custom_target(
    output,
    input: inputs,
    output: output,
    command: [xsltproc, '--output', '@OUTPUT@', '@INPUT0@', '@INPUT1@'],
    build_by_default: true,
  )
endif

content_files += configure_file(
  input: 'version.xml.in',
  output: '@BASENAME@',
  configuration: data_conf,
)

filecopier = find_program('cp')
foreach intro: introspection_files
  output = intro[1].split('/')[-1]

  content_files += custom_target(
    output,
    input: intro[0],
    output: output,
    command: [filecopier, intro[1], '@OUTPUT@'],
    build_by_default: true,
  )
endforeach

output = 'common.ent'

content_files += custom_target(
  output,
  input: common_ent_file,
  output: output,
  command: [filecopier, '@INPUT@', '@OUTPUT@'],
  build_by_default: true,
)

gnome.gtkdoc(
  nm_name,
  main_xml: 'network-manager-docs.xml',
  src_dir: meson.current_source_dir(),
  content_files: content_files,
  install: true,
)
