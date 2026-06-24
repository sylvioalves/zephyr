# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_services)

zephyr_syscall_header(${CMAKE_CURRENT_SOURCE_DIR}/src/svc_broker.h)

target_sources(app PRIVATE
	src/main.c
	src/app_group.c
	src/svc_registry.c
	src/svc_broker.c
	src/net_link.c
)
