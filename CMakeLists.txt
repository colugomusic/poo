cmake_minimum_required(VERSION 4.3.1)
project(poo)

enable_testing()

add_library(poo INTERFACE)
add_library(poo::poo ALIAS poo)

target_sources(poo INTERFACE
	FILE_SET HEADERS
	BASE_DIRS
		include/poo
	FILES
		include/poo/poo.hpp
)

if (BUILD_TESTING)
	add_subdirectory(test)
endif()

include(CMakePackageConfigHelpers)
install(TARGETS poo EXPORT poo-targets FILE_SET HEADERS DESTINATION include/poo)
install(EXPORT poo-targets FILE poo-targets.cmake NAMESPACE poo:: DESTINATION lib/cmake/poo)
set(config_in "${CMAKE_CURRENT_LIST_DIR}/cmake/poo-config.cmake.in")
set(config_out "${CMAKE_CURRENT_BINARY_DIR}/poo-config.cmake")
configure_package_config_file(${config_in} ${config_out} INSTALL_DESTINATION lib/cmake/poo)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/poo-config.cmake" DESTINATION lib/cmake/poo)
