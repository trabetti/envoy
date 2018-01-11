# Raw release notes

This file contains "raw" release notes for each release. The notes are added by developers as changes
are made to Envoy that are user impacting. When a release is cut, the releaser will use these notes
to populate the [Sphinx release notes in data-plane-api](https://github.com/envoyproxy/data-plane-api/blob/master/docs/root/intro/version_history.rst).
The idea is that this is a low friction way to add release notes along with code changes. Doing this
will make it substantially easier for the releaser to "linkify" all of the release notes in the
final version.

## 1.6.0
* Added gRPC access logging.
* Added DOWNSTREAM_REMOTE_ADDRESS, DOWNSTREAM_REMOTE_ADDRESS_WITHOUT_PORT, and
  DOWNSTREAM_LOCAL_ADDRESS access log formatters. DOWNSTREAM_ADDRESS access log formatter has been
  deprecated.
* TCP proxy access logs now bring an IP address without a port when using DOWNSTREAM_ADDRESS.
  Use DOWNSTREAM_REMOTE_ADDRESS instead.
* Added DOWNSTREAM_REMOTE_ADDRESS_WITHOUT_PORT header formatter. CLIENT_IP header formatter has been
  deprecated.
* Added transport socket interface to allow custom implementation of transport socket. A transport socket
  provides read and write logic with buffer encryption and decryption. The existing TLS implementation is
  refactored with the interface.
* Added support for dynamic response header values (`%CLIENT_IP%` and `%PROTOCOL%`).
* Added native DogStatsD support. :ref:`DogStatsdSink <envoy_api_msg_DogStatsdSink>`
* grpc-json: Added support inline descriptor in config.
* Added support for listening for both IPv4 and IPv6 when binding to ::.
* Added support for :ref:`LocalityLbEndpoints<envoy_api_msg_LocalityLbEndpoints>` priorities.
* Added idle timeout to TCP proxy.
* Added support for dynamic headers generated from upstream host endpoint metadata
  (`UPSTREAM_METADATA(...)`).
* Added restrictions for the backing sources of xDS resources. For filesystem based
xDS the file must exist at configuration time. For cluster based xDS (api\_config\_source, and ADS) the backing cluster must be statically defined and be of non-EDS type.
* Added support for route matching based on URL query string parameters.
  :ref:`QueryParameterMatcher<envoy_api_msg_QueryParameterMatcher>`
