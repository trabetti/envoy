licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "hystrix_lib",
    srcs = ["hystrix.cc"],
    hdrs = ["hystrix.h"],
    deps = [
        "//include/envoy/server:admin_interface",
        "//include/envoy/server:instance_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/stats:hystrix_stats_lib",        
    ],
)