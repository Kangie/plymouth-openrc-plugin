project(
    'plymouth-openrc-plugin',
    'c',
    version: '0.3.0',
    default_options: [
        'prefix=/usr',
    ],
)

einfo = dependency('einfo', required: true)
openrc = dependency('openrc', version: '>=0.55', required: true )

dependencies = [einfo, openrc]

conf = configuration_data()
conf.set('DEBUG', get_option('debug'))
conf.set_quoted('PID_FILE', get_option('pid-file'))
conf.set_quoted('RUN_DIR', get_option('run-dir'))

configure_file(
    output: 'config.h',
    configuration: conf,
)

plugin = shared_library(
    'plymouth',
    'plymouth.c',
    dependencies: dependencies,
    name_prefix: '',
    install: true,
    install_dir: openrc.get_pkgconfig_variable('rc_path') / 'plugins',
)
