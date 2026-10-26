#include <stdexcept>

#include "nuvo/client.hpp"
namespace nuvo::client {
std::unique_ptr<Source> https_source(const HttpsOptions&) {
  throw std::runtime_error("SDK built without HTTPS");
}
}  // namespace nuvo::client
