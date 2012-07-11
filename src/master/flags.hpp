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

#ifndef __MASTER_FLAGS_HPP__
#define __MASTER_FLAGS_HPP__

#include "flags/flags.hpp"

namespace mesos {
namespace internal {
namespace master {

class Flags : public flags::FlagsBase
{
public:
  Flags()
  {
    add(&root_submissions,
        "root_submissions",
        "Can root submit frameworks?",
        true);

    add(&slaves,
        "slaves",
        "Initial slaves that should be "
        "considered part of this cluster "
        "(or if using ZooKeeper a URL)",
        "*");

    add(&webui_dir,
        "webui_dir",
        "Directory from which to serve files "
        "for the webui",
        MESOS_WEBUI_DIR);

    add(&whitelist,
        "whitelist",
        "Path to a file with a list of slaves "
        "(one per line) to advertise offers for; "
        "should be of the form: file://path/to/file",
        "*");
        
  }

  bool root_submissions;
  std::string slaves;
  std::string webui_dir;
  std::string whitelist;
};

} // namespace mesos {
} // namespace internal {
} // namespace master {

#endif // __MASTER_FLAGS_HPP__
