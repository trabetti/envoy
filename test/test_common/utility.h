#pragma once

#include <stdlib.h>

#include <condition_variable>
#include <list>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "envoy/buffer/buffer.h"
#include "envoy/network/address.h"
#include "envoy/stats/stats.h"

#include "common/http/header_map_impl.h"
#include "common/protobuf/utility.h"

#include "test/test_common/printers.h"

#include "api/bootstrap.pb.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::AssertionFailure;
using testing::AssertionResult;
using testing::AssertionSuccess;

namespace Envoy {
#define EXPECT_THROW_WITH_MESSAGE(statement, expected_exception, message)                          \
  try {                                                                                            \
    statement;                                                                                     \
    ADD_FAILURE() << "Exception should take place. It did not.";                                   \
  } catch (expected_exception & e) {                                                               \
    EXPECT_EQ(message, std::string(e.what()));                                                     \
  }

#define EXPECT_THROW_WITH_REGEX(statement, expected_exception, regex_str)                          \
  try {                                                                                            \
    statement;                                                                                     \
    ADD_FAILURE() << "Exception should take place. It did not.";                                   \
  } catch (expected_exception & e) {                                                               \
    EXPECT_THAT(e.what(), ::testing::ContainsRegex(regex_str));                                    \
  }

#define VERBOSE_EXPECT_NO_THROW(statement)                                                         \
  try {                                                                                            \
    statement;                                                                                     \
  } catch (EnvoyException & e) {                                                                   \
    ADD_FAILURE() << "Unexpected exception: " << std::string(e.what());                            \
  }

// Random number generator which logs its seed to stderr. To repeat a test run with a non-zero seed
// one can run the test with --test_arg=--gtest_random_seed=[seed]
class TestRandomGenerator {
public:
  TestRandomGenerator();

  uint64_t random();

private:
  const int32_t seed_;
  std::ranlux48 generator_;
};

class TestUtility {
public:
  /**
   * Compare 2 buffers.
   * @param lhs supplies buffer 1.
   * @param rhs supplies buffer 2.
   * @return TRUE if the buffers are equal, false if not.
   */
  static bool buffersEqual(const Buffer::Instance& lhs, const Buffer::Instance& rhs);

  /**
   * Convert a buffer to a string.
   * @param buffer supplies the buffer to convert.
   * @return std::string the converted string.
   */
  static std::string bufferToString(const Buffer::Instance& buffer);

  /**
   * Feed a buffer with random characters.
   * @param buffer supplies the buffer to be fed.
   * @param n_char number of characters that should be added to the supplied buffer.
   * @param seed seeds pseudo-random number genarator (default = 0).
   */
  static void feedBufferWithRandomCharacters(Buffer::Instance& buffer, uint64_t n_char,
                                             uint64_t seed = 0);

  /**
   * Find a counter in a stats store.
   * @param store supplies the stats store.
   * @param name supplies the name to search for.
   * @return Stats::CounterSharedPtr the counter or nullptr if there is none.
   */
  static Stats::CounterSharedPtr findCounter(Stats::Store& store, const std::string& name);

  /**
   * Find a gauge in a stats store.
   * @param store supplies the stats store.
   * @param name supplies the name to search for.
   * @return Stats::GaugeSharedPtr the gauge or nullptr if there is none.
   */
  static Stats::GaugeSharedPtr findGauge(Stats::Store& store, const std::string& name);

  /**
   * Convert a string list of IP addresses into a list of network addresses usable for DNS
   * response testing.
   */
  static std::list<Network::Address::InstanceConstSharedPtr>
  makeDnsResponse(const std::list<std::string>& addresses);

  /**
   * List files in a given directory path
   *
   * @param path directory path to list
   * @param recursive whether or not to traverse subdirectories
   * @return std::vector<std::string> filenames
   */
  static std::vector<std::string> listFiles(const std::string& path, bool recursive);

  /**
   * Compare two protos of the same type for equality.
   *
   * @param lhs proto on LHS.
   * @param rhs proto on RHS.
   * @return bool indicating whether the protos are equal. Type name and string serialization are
   *         used for equality testing.
   */
  static bool protoEqual(const Protobuf::Message& lhs, const Protobuf::Message& rhs) {
    return lhs.GetTypeName() == rhs.GetTypeName() &&
           lhs.SerializeAsString() == rhs.SerializeAsString();
  }

  /**
   * Split a string.
   * @param source supplies the string to split.
   * @param split supplies the char to split on.
   * @return vector of strings computed after splitting `source` around all instances of `split`.
   */
  static std::vector<std::string> split(const std::string& source, char split);

  /**
   * Split a string.
   * @param source supplies the string to split.
   * @param split supplies the string to split on.
   * @param keep_empty_string result contains empty strings if the string starts or ends with
   * 'split', or if instances of 'split' are adjacent.
   * @return vector of strings computed after splitting `source` around all instances of `split`.
   */
  static std::vector<std::string> split(const std::string& source, const std::string& split,
                                        bool keep_empty_string = false);

  /**
   * Compare two RepeatedPtrFields of the same type for equality.
   *
   * @param lhs RepeatedPtrField on LHS.
   * @param rhs RepeatedPtrField on RHS.
   * @return bool indicating whether the RepeatedPtrField are equal. TestUtility::protoEqual() is
   *              used for individual element testing.
   */
  template <class ProtoType>
  static bool repeatedPtrFieldEqual(const Protobuf::RepeatedPtrField<ProtoType>& lhs,
                                    const Protobuf::RepeatedPtrField<ProtoType>& rhs) {
    if (lhs.size() != rhs.size()) {
      return false;
    }

    for (int i = 0; i < lhs.size(); ++i) {
      if (!TestUtility::protoEqual(lhs[i], rhs[i])) {
        return false;
      }
    }

    return true;
  }

  template <class ProtoType>
  static AssertionResult
  assertRepeatedPtrFieldEqual(const Protobuf::RepeatedPtrField<ProtoType>& lhs,
                              const Protobuf::RepeatedPtrField<ProtoType>& rhs) {
    if (!repeatedPtrFieldEqual(lhs, rhs)) {
      return AssertionFailure() << RepeatedPtrUtil::debugString(lhs) << " does not match "
                                << RepeatedPtrUtil::debugString(rhs);
    }

    return AssertionSuccess();
  }

  /**
   * Parse bootstrap config from v1 JSON static config string.
   * @param json_string source v1 JSON static config string.
   * @return envoy::api::v2::Bootstrap.
   */
  static envoy::api::v2::Bootstrap parseBootstrapFromJson(const std::string& json_string);

  /**
   * Returns a "novel" IPv4 loopback address, if available.
   * For many tests, we want a loopback address other than 127.0.0.1 where possible. For some
   * platforms such as OSX, only 127.0.0.1 is available for IPv4 loopback.
   *
   * @return string 127.0.0.x , where x is "1" for OSX and "9" otherwise.
   */
  static std::string getIpv4Loopback() {
#ifdef __APPLE__
    return "127.0.0.1";
#else
    return "127.0.0.9";
#endif
  }

  /**
   * Return typed proto message object for YAML.
   * @param yaml YAML string.
   * @return MessageType parsed from yaml.
   */
  template <class MessageType> static MessageType parseYaml(const std::string& yaml) {
    MessageType message;
    MessageUtil::loadFromYaml(yaml, message);
    return message;
  }
};

/**
 * This utility class wraps the common case of having a cross-thread "one shot" ready condition.
 */
class ConditionalInitializer {
public:
  /**
   * Set the conditional to ready.
   */
  void setReady();

  /**
   * Block until the conditional is ready, will return immediately if it is already ready. This
   * routine will also reset ready_ so that the initializer can be used again. setReady() should
   * only be called once in between a call to waitReady().
   */
  void waitReady();

private:
  std::condition_variable cv_;
  std::mutex mutex_;
  bool ready_{false};
};

class ScopedFdCloser {
public:
  ScopedFdCloser(int fd);
  ~ScopedFdCloser();

private:
  int fd_;
};

namespace Http {

/**
 * A test version of HeaderMapImpl that adds some niceties around letting us use
 * std::string instead of always doing LowerCaseString() by hand.
 */
class TestHeaderMapImpl : public HeaderMapImpl {
public:
  TestHeaderMapImpl();
  TestHeaderMapImpl(const std::initializer_list<std::pair<std::string, std::string>>& values);
  TestHeaderMapImpl(const HeaderMap& rhs);

  friend std::ostream& operator<<(std::ostream& os, const TestHeaderMapImpl& p) {
    p.iterate(
        [](const HeaderEntry& header, void* context) -> HeaderMap::Iterate {
          std::ostream* local_os = static_cast<std::ostream*>(context);
          *local_os << header.key().c_str() << " " << header.value().c_str() << std::endl;
          return HeaderMap::Iterate::Continue;
        },
        &os);
    return os;
  }

  using HeaderMapImpl::addCopy;
  void addCopy(const std::string& key, const std::string& value);
  std::string get_(const std::string& key);
  std::string get_(const LowerCaseString& key);
  bool has(const std::string& key);
  bool has(const LowerCaseString& key);
};

} // namespace Http

MATCHER_P(ProtoEq, rhs, "") { return TestUtility::protoEqual(arg, rhs); }

MATCHER_P(RepeatedProtoEq, rhs, "") { return TestUtility::repeatedPtrFieldEqual(arg, rhs); }

} // Envoy
