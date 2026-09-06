/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nanobind/stl/string.h>
#include <vlink/extension/security.h>
#include <vlink/impl/ssl_options.h>

#include "buffer.h"
#include "callbacks.h"

namespace vlink::python {

using namespace nb::literals;  // NOLINT

static auto make_security_callback(nb::callable py_cb, const char* context) {
  auto cb = std::make_shared<GilSafePyFunction>(std::move(py_cb));

  return [cb = std::move(cb), context](const vlink::Bytes& in, vlink::Bytes& out) -> bool {
    if VUNLIKELY (!Py_IsInitialized()) {
      return false;
    }

    nb::gil_scoped_acquire gil;

    try {
      nb::object result = cb->fn(PythonCodec<vlink::Bytes>::to_python(in));

      if VUNLIKELY (result.is_none()) {
        return false;
      }

      out = PythonCodec<vlink::Bytes>::from_python_owned(result);
      return true;
    } catch (std::exception&) {
      report_current_exception(context);
      return false;
    }
  };
}

void bind_security(nb::module_& m) {
  nb::class_<vlink::SslOptions>(m, "SslOptions")
      .def(nb::init<>())
      .def_rw("verify_peer", &vlink::SslOptions::verify_peer)
      .def_rw("ca_file", &vlink::SslOptions::ca_file)
      .def_rw("cert_file", &vlink::SslOptions::cert_file)
      .def_rw("key_file", &vlink::SslOptions::key_file)
      .def_rw("key_password", &vlink::SslOptions::key_password)
      .def_rw("server_name", &vlink::SslOptions::server_name)
      .def_rw("ciphers", &vlink::SslOptions::ciphers)
      .def("is_valid", &vlink::SslOptions::is_valid);

  nb::class_<vlink::Security::Config::Advanced>(
      m, "SecurityConfigAdvanced", "Low-frequency security options for AAD, replay protection, and signing")
      .def(nb::init<>())
      .def_rw("aad_context", &vlink::Security::Config::Advanced::aad_context)
      .def_rw("replay_window", &vlink::Security::Config::Advanced::replay_window)
      .def_rw("signing_key_pem", &vlink::Security::Config::Advanced::signing_key_pem)
      .def_rw("verify_key_pem", &vlink::Security::Config::Advanced::verify_key_pem);

  nb::class_<vlink::Security::Config>(m, "SecurityConfig",
                                      "Aggregate of every parameter accepted by the Security constructor")
      .def(nb::init<>())
      .def_rw("key", &vlink::Security::Config::key)
      .def_rw("passphrase", &vlink::Security::Config::passphrase)
      .def_rw("pbkdf2_salt", &vlink::Security::Config::pbkdf2_salt)
      .def_rw("pbkdf2_iterations", &vlink::Security::Config::pbkdf2_iterations)
      .def_rw("public_key_pem", &vlink::Security::Config::public_key_pem)
      .def_rw("private_key_pem", &vlink::Security::Config::private_key_pem)
      .def_rw("advanced", &vlink::Security::Config::advanced)
      .def_prop_rw(
          "encrypt_callback",
          [](const vlink::Security::Config& self) -> nb::object {
            return self.encrypt_callback ? nb::cast(true) : nb::none();
          },
          [](vlink::Security::Config& self, nb::callable cb) {
            self.encrypt_callback = make_security_callback(std::move(cb), "vlink::Security::Config.encrypt_callback");
          })
      .def_prop_rw(
          "decrypt_callback",
          [](const vlink::Security::Config& self) -> nb::object {
            return self.decrypt_callback ? nb::cast(true) : nb::none();
          },
          [](vlink::Security::Config& self, nb::callable cb) {
            self.decrypt_callback = make_security_callback(std::move(cb), "vlink::Security::Config.decrypt_callback");
          });

  nb::class_<vlink::Security>(m, "Security", "Authenticated message-level encryption (AEAD)")
      .def(nb::new_([](vlink::Security::Config cfg) { return new vlink::Security(std::move(cfg)); }),
           "cfg"_a = vlink::Security::Config{})
      .def_static("from_private_key_path", &vlink::Security::from_private_key_path, "private_key_path"_a,
                  "Create a SecurityConfig by reading a private-key PEM file.")
      .def_static("from_public_key_path", &vlink::Security::from_public_key_path, "public_key_path"_a,
                  "Create a SecurityConfig by reading a public-key PEM file.")
      .def_static("from_key_paths", &vlink::Security::from_key_paths, "public_key_path"_a, "private_key_path"_a,
                  "Create a SecurityConfig by reading public- and private-key PEM files.")
      .def(
          "encrypt",
          [](vlink::Security& self, nb::handle data) -> nb::object {
            PythonBufferView view(data);
            auto in_bytes = vlink::Bytes::shallow_copy(view.data(), view.size());
            vlink::Bytes out;

            if VLIKELY (self.encrypt(in_bytes, out)) {
              return PythonCodec<vlink::Bytes>::to_python(out);
            }

            return nb::none();
          },
          "data"_a)
      .def(
          "decrypt",
          [](vlink::Security& self, nb::handle data) -> nb::object {
            PythonBufferView view(data);
            auto in_bytes = vlink::Bytes::shallow_copy(view.data(), view.size());
            vlink::Bytes out;

            if VLIKELY (self.decrypt(in_bytes, out)) {
              return PythonCodec<vlink::Bytes>::to_python(out);
            }

            return nb::none();
          },
          "data"_a)
      .def("is_configured", &vlink::Security::is_configured,
           "Return True iff at least one cryptographic slot (symmetric key, RSA keypair, or "
           "encrypt+decrypt callback pair) is usable.")
      .def("can_encrypt", &vlink::Security::can_encrypt,
           "Return True iff encrypt() will produce a ciphertext for at least one configured mode "
           "(custom callbacks > RSA public key > symmetric key).")
      .def("can_decrypt", &vlink::Security::can_decrypt,
           "Return True iff decrypt() can recover a plaintext for at least one configured mode "
           "(custom callbacks > RSA private key > symmetric key).");
}

}  // namespace vlink::python
