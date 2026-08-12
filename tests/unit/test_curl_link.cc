#include "tb.h"
#include <gtest/gtest.h>
#include <curl/curl.h>

TEST(Curl, VersionAndBackendIsMbedtls) {
  curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
  ASSERT_NE(v, nullptr);
  // ssl_version 形如 "mbedTLS/x.y.z"。绝不允许 OpenSSL。
  ASSERT_NE(std::string(v->ssl_version).find("mbedTLS"), std::string::npos)
      << "TLS backend must be mbedTLS, got: " << v->ssl_version;
}

TEST(Curl, EasyInit) {
  CURL *c = curl_easy_init();
  ASSERT_NE(c, nullptr);
  curl_easy_cleanup(c);
}
