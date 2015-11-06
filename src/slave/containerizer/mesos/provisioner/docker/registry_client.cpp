/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>
#include <ostream>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os/close.hpp>
#include <stout/os/mkdir.hpp>
#include <stout/os/open.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/mesos/provisioner/docker/registry_client.hpp"
#include "slave/containerizer/mesos/provisioner/docker/token_manager.hpp"

using std::ostringstream;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;

namespace http = process::http;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace registry {


const Duration RegistryClient::DEFAULT_MANIFEST_TIMEOUT_SECS = Seconds(10);
const size_t RegistryClient::DEFAULT_MANIFEST_MAXSIZE_BYTES = 4096;
static const uint16_t DEFAULT_SSL_PORT = 443;


class RegistryClientProcess : public Process<RegistryClientProcess>
{
public:
  static Try<Owned<RegistryClientProcess>> create(
      const http::URL& registry,
      const http::URL& authServer,
      const Option<Credentials>& creds);

  Future<Manifest> getManifest(
      const string& path,
      const Option<string>& tag,
      const Duration& timeout);

  Future<size_t> getBlob(
      const string& path,
      const Option<string>& digest,
      const Path& filePath,
      const Duration& timeout,
      size_t maxSize);

private:
  RegistryClientProcess(
    const http::URL& registryServer,
    const Owned<TokenManager>& tokenManager,
    const Option<Credentials>& creds);

  Future<http::Response> doHttpGet(
      const http::URL& url,
      const Option<http::Headers>& headers,
      const Duration& timeout,
      bool resend,
      const Option<string>& lastResponse) const;

  Try<http::Headers> getAuthenticationAttributes(
      const http::Response& httpResponse) const;

  const http::URL registryServer_;
  Owned<TokenManager> tokenManager_;
  const Option<Credentials> credentials_;

  RegistryClientProcess(const RegistryClientProcess&) = delete;
  RegistryClientProcess& operator = (const RegistryClientProcess&) = delete;
};


Try<Owned<RegistryClient>> RegistryClient::create(
    const http::URL& registryServer,
    const http::URL& authServer,
    const Option<Credentials>& creds)
{
  Try<Owned<RegistryClientProcess>> process =
    RegistryClientProcess::create(authServer, registryServer, creds);

  if (process.isError()) {
    return Error(process.error());
  }

  return Owned<RegistryClient>(
      new RegistryClient(authServer, registryServer, creds, process.get()));
}


RegistryClient::RegistryClient(
    const http::URL& registryServer,
    const http::URL& authServer,
    const Option<Credentials>& creds,
    const Owned<RegistryClientProcess>& process)
  : registryServer_(registryServer),
    authServer_(authServer),
    credentials_(creds),
    process_(process)
{
  spawn(CHECK_NOTNULL(process_.get()));
}


RegistryClient::~RegistryClient()
{
  terminate(process_.get());
  process::wait(process_.get());
}


Future<Manifest> RegistryClient::getManifest(
    const string& _path,
    const Option<string>& _tag,
    const Option<Duration>& _timeout)
{
  Duration timeout = _timeout.getOrElse(DEFAULT_MANIFEST_TIMEOUT_SECS);

  return dispatch(
      process_.get(),
      &RegistryClientProcess::getManifest,
      _path,
      _tag,
      timeout);
}


Future<size_t> RegistryClient::getBlob(
    const string& _path,
    const Option<string>& _digest,
    const Path& _filePath,
    const Option<Duration>& _timeout,
    const Option<size_t>& _maxSize)
{
  Duration timeout = _timeout.getOrElse(DEFAULT_MANIFEST_TIMEOUT_SECS);
  size_t maxSize = _maxSize.getOrElse(DEFAULT_MANIFEST_MAXSIZE_BYTES);

  return dispatch(
        process_.get(),
        &RegistryClientProcess::getBlob,
        _path,
        _digest,
        _filePath,
        timeout,
        maxSize);
}


Try<Owned<RegistryClientProcess>> RegistryClientProcess::create(
    const http::URL& registryServer,
    const http::URL& authServer,
    const Option<Credentials>& creds)
{
  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(authServer);
  if (tokenMgr.isError()) {
    return Error("Failed to create token manager: " + tokenMgr.error());
  }

  return Owned<RegistryClientProcess>(
      new RegistryClientProcess(registryServer, tokenMgr.get(), creds));
}


RegistryClientProcess::RegistryClientProcess(
    const http::URL& registryServer,
    const Owned<TokenManager>& tokenMgr,
    const Option<Credentials>& creds)
  : registryServer_(registryServer),
    tokenManager_(tokenMgr),
    credentials_(creds) {}


Try<http::Headers> RegistryClientProcess::getAuthenticationAttributes(
    const http::Response& httpResponse) const
{
  if (httpResponse.headers.find("WWW-Authenticate") ==
      httpResponse.headers.end()) {
    return Error("Failed to find WWW-Authenticate header value");
  }

  const string& authString = httpResponse.headers.at("WWW-Authenticate");

  const vector<string> authStringTokens = strings::tokenize(authString, " ");
  if ((authStringTokens.size() != 2) || (authStringTokens[0] != "Bearer")) {
    // TODO(jojy): Look at various possibilities of auth response. We currently
    // assume that the string will have realm information.
    return Error("Invalid authentication header value: " + authString);
  }

  const vector<string> authParams = strings::tokenize(authStringTokens[1], ",");

  http::Headers authAttributes;
  auto addAttribute = [&authAttributes](
      const string& param) -> Try<Nothing> {
    const vector<string> paramTokens =
      strings::tokenize(param, "=\"");

    if (paramTokens.size() != 2) {
      return Error(
          "Failed to get authentication attribute from response parameter " +
          param);
    }

    authAttributes.insert({paramTokens[0], paramTokens[1]});

    return Nothing();
  };

  foreach (const string& param, authParams) {
    Try<Nothing> addRes = addAttribute(param);
    if (addRes.isError()) {
      return Error(addRes.error());
    }
  }

  return authAttributes;
}


Future<http::Response> RegistryClientProcess::doHttpGet(
    const http::URL& url,
    const Option<http::Headers>& headers,
    const Duration& timeout,
    bool resend,
    const Option<string>& lastResponseStatus) const
{
  return http::get(url, headers)
    .after(timeout, [](const Future<http::Response>& httpResponseFuture)
        -> Future<http::Response> {
      return Failure("Response timeout");
    })
    .then(defer(self(), [=](const http::Response& httpResponse)
        -> Future<http::Response> {
      VLOG(1) << "Response status: " + httpResponse.status;

      // Set the future if we get a OK response.
      if (httpResponse.status == "200 OK") {
        return httpResponse;
      } else if (httpResponse.status == "400 Bad Request") {
        Try<JSON::Object> errorResponse =
          JSON::parse<JSON::Object>(httpResponse.body);

        if (errorResponse.isError()) {
          return Failure("Failed to parse bad request response JSON: " +
                         errorResponse.error());
        }

        ostringstream out;
        bool first = true;
        Result<JSON::Array> errorObjects =
          errorResponse.get().find<JSON::Array>("errors");

        if (errorObjects.isError()) {
          return Failure("Failed to find 'errors' in bad request response: " +
                         errorObjects.error());
        } else if (errorObjects.isNone()) {
          return Failure("Errors not found in bad request response");
        }

        foreach (const JSON::Value& error, errorObjects.get().values) {
          Result<JSON::String> message =
            error.as<JSON::Object>().find<JSON::String>("message");

          if (message.isError()) {
            return Failure("Failed to parse bad request error message: " +
                           message.error());
          } else if (message.isNone()) {
            continue;
          }

          if (first) {
            out << message.get().value;
            first = false;
          } else {
            out << ", " << message.get().value;
          }
        }

        return Failure("Received Bad request, errors: [" + out.str() + "]");
      }

      // Prevent infinite recursion.
      if (lastResponseStatus.isSome() &&
          (lastResponseStatus.get() == httpResponse.status)) {
        return Failure("Invalid response: " + httpResponse.status);
      }

      // If resend is not set, we dont try again and stop here.
      if (!resend) {
        return Failure("Bad response: " + httpResponse.status);
      }

      // Handle 401 Unauthorized.
      if (httpResponse.status == "401 Unauthorized") {
        Try<http::Headers> authAttributes =
          getAuthenticationAttributes(httpResponse);

        if (authAttributes.isError()) {
          return Failure(
              "Failed to get authentication attributes: " +
              authAttributes.error());
        }

        if (!authAttributes.get().contains("service")) {
          return Failure(
              "Failed to find authentication attribute \"service\" in response"
              "from authorization server");
        }

        if (!authAttributes.get().contains("scope")) {
          return Failure(
              "Failed to find authentication attribute \"scope\" in response"
              "from authorization server");
        }

        // TODO(jojy): Currently only handling TLS/cert authentication.
        Future<Token> tokenResponse = tokenManager_->getToken(
          authAttributes.get().at("service"),
          authAttributes.get().at("scope"),
          None());

        return tokenResponse
          .after(timeout, [=](
              Future<Token> tokenResponse) -> Future<Token> {
            tokenResponse.discard();
            return Failure("Token response timeout");
          })
          .then(defer(self(), [=](
              const Future<Token>& tokenResponse) {
            // Send request with acquired token.
            http::Headers authHeaders = {
              {"Authorization", "Bearer " + tokenResponse.get().raw}
            };

            return doHttpGet(
                url,
                authHeaders,
                timeout,
                true,
                httpResponse.status);
        }));
      } else if (httpResponse.status == "307 Temporary Redirect") {
        // Handle redirect.

        // TODO(jojy): Add redirect functionality in http::get.

        auto toURL = [](
            const string& urlString) -> Try<http::URL> {
          // TODO(jojy): Need to add functionality to URL class that parses a
          // string to its URL components. For now, assuming:
          //  - scheme is https
          //  - path always ends with /

          static const string schemePrefix = "https://";

          if (!strings::contains(urlString, schemePrefix)) {
            return Error(
                "Failed to find expected token '" + schemePrefix +
                "' in redirect url");
          }

          const string schemeSuffix = urlString.substr(schemePrefix.length());

          const vector<string> components =
            strings::tokenize(schemeSuffix, "/");

          const string path = schemeSuffix.substr(components[0].length());

          const vector<string> addrComponents =
            strings::tokenize(components[0], ":");

          uint16_t port = DEFAULT_SSL_PORT;
          string domain = components[0];

          // Parse the port.
          if (addrComponents.size() == 2) {
            domain = addrComponents[0];

            Try<uint16_t> tryPort = numify<uint16_t>(addrComponents[1]);
            if (tryPort.isError()) {
              return Error(
                  "Failed to parse location: " + urlString + " for port.");
            }

            port = tryPort.get();
          }

          return http::URL("https", domain, port, path);
        };

        if (httpResponse.headers.find("Location") ==
            httpResponse.headers.end()) {
          return Failure(
              "Invalid redirect response: 'Location' not found in headers.");
        }

        const string& location = httpResponse.headers.at("Location");
        Try<http::URL> tryUrl = toURL(location);
        if (tryUrl.isError()) {
          return Failure(
              "Failed to parse '" + location + "': " + tryUrl.error());
        }

        return doHttpGet(
            tryUrl.get(),
            headers,
            timeout,
            false,
            httpResponse.status);
      } else {
        return Failure("Invalid response: " + httpResponse.status);
      }
    }));
}


Future<Manifest> RegistryClientProcess::getManifest(
    const string& path,
    const Option<string>& tag,
    const Duration& timeout)
{
  if (strings::contains(path, " ")) {
    return Failure("Invalid repository path: " + path);
  }

  string repoTag = tag.getOrElse("latest");
  if (strings::contains(repoTag, " ")) {
    return Failure("Invalid repository tag: " + repoTag);
  }

  http::URL manifestURL(registryServer_);
  manifestURL.path =
    "v2/" + path + "/manifests/" + repoTag;

  auto getManifest = [](const http::Response& httpResponse) -> Try<Manifest> {
    if (!httpResponse.headers.contains("Docker-Content-Digest")) {
      return Error("Docker-Content-Digest header missing in response");
    }

    Try<JSON::Object> responseJSON =
      JSON::parse<JSON::Object>(httpResponse.body);

    if (responseJSON.isError()) {
      return Error(responseJSON.error());
    }

    Result<JSON::String> name = responseJSON.get().find<JSON::String>("name");
    if (name.isNone()) {
      return Error("Failed to find \"name\" in manifest response");
    }

    Result<JSON::Array> fsLayers =
      responseJSON.get().find<JSON::Array>("fsLayers");

    if (fsLayers.isNone()) {
      return Error("Failed to find \"fsLayers\" in manifest response");
    }

    Result<JSON::Array> historyArray =
      responseJSON.get().find<JSON::Array>("history");

    if (historyArray.isNone()) {
      return Error("Failed to find \"history\" in manifest response");
    }

    if (historyArray.get().values.size() != fsLayers.get().values.size()) {
      return Error(
          "\"history\" and \"fsLayers\" array count mismatch"
          "in manifest response");
    }

    vector<FileSystemLayerInfo> fsLayerInfoList;
    size_t index = 0;

    foreach (const JSON::Value& layer, fsLayers.get().values) {
      if (!layer.is<JSON::Object>()) {
        return Error(
            "Failed to parse layer as a JSON object for index: " +
            stringify(index));
      }

      const JSON::Object& layerInfoJSON = layer.as<JSON::Object>();

      // Get blobsum for layer.
      const Result<JSON::String> blobSumInfo =
        layerInfoJSON.find<JSON::String>("blobSum");

      if (blobSumInfo.isNone()) {
        return Error("Failed to find \"blobSum\" in manifest response");
      }

      // Get history for layer.
      if (!historyArray.get().values[index].is<JSON::Object>()) {
        return Error(
            "Failed to parse history as a JSON object for index: " +
            stringify(index));
      }
      const JSON::Object& historyObj =
        historyArray.get().values[index].as<JSON::Object>();

      // Get layer id.
      const Result<JSON::String> v1CompatibilityJSON =
        historyObj.find<JSON::String>("v1Compatibility");

      if (!v1CompatibilityJSON.isSome()) {
        return Error(
            "Failed to obtain layer v1 compability json in manifest for layer: "
            + stringify(index));
      }

      Try<JSON::Object> v1CompatibilityObj =
        JSON::parse<JSON::Object>(v1CompatibilityJSON.get().value);

      if (!v1CompatibilityObj.isSome()) {
        return Error(
            "Failed to parse v1 compability json in manifest for layer: "
            + stringify(index));
      }

      const Result<JSON::String> id =
        v1CompatibilityObj.get().find<JSON::String>("id");

      if (!id.isSome()) {
        return Error(
            "Failed to find \"id\" in manifest for layer: " + stringify(index));
      }

      fsLayerInfoList.emplace_back(
          FileSystemLayerInfo{
            blobSumInfo.get().value,
            id.get().value,
          });

      index++;
    }

    return Manifest {
      name.get().value,
      httpResponse.headers.at("Docker-Content-Digest"),
      fsLayerInfoList,
    };
  };

  return doHttpGet(manifestURL, None(), timeout, true, None())
    .then([getManifest] (const http::Response& response) -> Future<Manifest> {
      Try<Manifest> manifest = getManifest(response);

      if (manifest.isError()) {
        return Failure(
            "Failed to parse manifest response: " + manifest.error());
      }

      return manifest.get();
    });
}


Future<size_t> RegistryClientProcess::getBlob(
    const string& path,
    const Option<string>& digest,
    const Path& filePath,
    const Duration& timeout,
    size_t maxSize)
{
  auto prepare = ([&filePath]() -> Try<Nothing> {
      const string dirName = filePath.dirname();

      // TODO(jojy): Return more state, for example - if the directory is new.
      Try<Nothing> dirResult = os::mkdir(dirName, true);
      if (dirResult.isError()) {
        return Error(
            "Failed to create directory to download blob: " +
            dirResult.error());
      }

      return dirResult;
  })();

  // TODO(jojy): This currently leaves a residue in failure cases. Would be
  // ideal if we can completely rollback.
  if (prepare.isError()) {
     return Failure(prepare.error());
  }

  if (strings::contains(path, " ")) {
    return Failure("Invalid repository path: " + path);
  }

  http::URL blobURL(registryServer_);
  blobURL.path =
    "v2/" + path + "/blobs/" + digest.getOrElse("");

  auto saveBlob = [filePath](const http::Response& httpResponse)
      -> Future<size_t> {
    // TODO(jojy): Add verification step.
    // TODO(jojy): Add check for max size.
    size_t size = httpResponse.body.length();
    Try<int> fd = os::open(
        filePath.value,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

    if (fd.isError()) {
      return Failure("Failed to open file '" + filePath.value + "': " +
                     fd.error());
    }

    return process::io::write(fd.get(), httpResponse.body)
      .then([size](const Future<Nothing>&) { return size; })
      .onAny([fd]() { os::close(fd.get()); } );
  };

  return doHttpGet(blobURL, None(), timeout, true, None())
    .then([saveBlob](const http::Response& response) {
      return saveBlob(response);
    });
}

} // namespace registry {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
