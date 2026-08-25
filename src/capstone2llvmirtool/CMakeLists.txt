
add_executable(capstone2llvmirtool
	capstone2llvmir.cpp
)

target_compile_features(capstone2llvmirtool PUBLIC cxx_std_17)

target_link_libraries(capstone2llvmirtool
	retdec::utils
	retdec::capstone2llvmir
	retdec::deps::keystone
)

set_target_properties(capstone2llvmirtool
	PROPERTIES
		OUTPUT_NAME "retdec-capstone2llvmir"
)

# Keystone (GPL-2.0) is linked only into this tool. Do not install it into
# commercial or cmake --install prefixes (LEG-11 / NOTICE).
# The target still builds for tests and developer use.
