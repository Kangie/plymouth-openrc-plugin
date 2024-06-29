project(
    'plymouth-openrc-plugin',
    'c',
    version: '0.2.0',
)

dependencies = [
    dependency('einfo', required: true),
    dependency('openrc', required: true),
]

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
    install_dir: join_paths(get_option('libdir'), 'rc' / 'plugins' / 'plymouth'),
)
