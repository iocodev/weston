if not get_option('backend-ebc')
	subdir_done()
endif

config_h.set('BUILD_EBC_COMPOSITOR', '1')

# The Rockchip rkebc SDK ships librkebc.so plus headers under
# <prefix>/include/rkebc/*.h.  In a buildroot SDK these are staged into the
# sysroot and this option can be left empty.
ebc_rkebc_prefix = get_option('ebc-rkebc-prefix')
ebc_rkebc_inc = []
ebc_rkebc_libdirs = []
if ebc_rkebc_prefix != ''
	ebc_rkebc_inc = include_directories(ebc_rkebc_prefix / 'include')
	ebc_rkebc_libdirs = [ ebc_rkebc_prefix / 'lib' ]
endif

dep_rkebc = cc.find_library('rkebc',
	required: true,
	dirs: ebc_rkebc_libdirs)

srcs_ebc = [
	'ebc.c',
	presentation_time_server_protocol_h,
]
plugin_ebc = shared_library(
	'ebc-backend',
	srcs_ebc,
	include_directories: [ common_inc, ebc_rkebc_inc ],
	dependencies: [
		dep_libweston_private,
		dep_libdrm_headers,
		dep_libinput_backend,
		dep_session_helper,
		dep_rkebc,
	],
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston,
)
env_modmap += 'ebc-backend.so=@0@;'.format(plugin_ebc.full_path())
install_headers(backend_ebc_h, subdir: dir_include_libweston_install)
