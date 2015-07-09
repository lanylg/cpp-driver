/*
  Copyright (c) 2014-2015 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_ERROR_RESPONSE_HPP_INCLUDED__
#define __CASS_ERROR_RESPONSE_HPP_INCLUDED__

#include "response.hpp"
#include "constants.hpp"
#include "scoped_ptr.hpp"
#include "string_ref.hpp"
#include "retry_policy.hpp"

#include <uv.h>

#include <string.h>
#include <string>

namespace cass {

class ErrorResponse : public Response {
public:
  ErrorResponse()
      : Response(CQL_OPCODE_ERROR)
      , code_(0xFFFFFFFF) { }

  int32_t code() const { return code_; }
  StringRef message() const { return message_; }

  StringRef prepared_id() const { return prepared_id_; }

  CassConsistency consistency() const {
    return static_cast<CassConsistency>(cl_);
  }
  int32_t received() const { return received_; }
  int32_t required() const { return required_; }
  int32_t alive() const { return alive_; }
  bool data_present() const { return data_present_; }
  RetryPolicy::WriteType write_type() const { return write_type_; }

  std::string error_message() const;

  bool decode(int version, char* buffer, size_t size);

private:
  int32_t code_;
  StringRef message_;

  StringRef prepared_id_;

  uint16_t cl_;
  int32_t received_;
  int32_t required_;
  int32_t alive_;
  bool data_present_;
  RetryPolicy::WriteType write_type_;
};

bool check_error_or_invalid_response(const std::string& prefix, uint8_t expected_opcode,
                                     Response* response);

} // namespace cass

#endif
