/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

#include "katana/HWTopo.h"
#include "katana/SimpleLock.h"
#include "katana/gIO.h"

#ifdef KATANA_USE_NUMA
#include <numa.h>
#endif

#ifdef KATANA_USE_SCHED_SETAFFINITY
#include <sched.h>
#endif

namespace {

struct cpuinfo {
  // fields filled in from OS files
  unsigned proc;
  unsigned physid;
  unsigned sib;
  unsigned coreid;
  unsigned cpucores;
  unsigned numaNode;  // from libnuma
  bool valid;         // from cpuset
  bool smt;           // computed
};

bool
operator<(const cpuinfo& lhs, const cpuinfo& rhs) {
  if (lhs.smt != rhs.smt)
    return lhs.smt < rhs.smt;
  if (lhs.physid != rhs.physid)
    return lhs.physid < rhs.physid;
  if (lhs.coreid != rhs.coreid)
    return lhs.coreid < rhs.coreid;
  return lhs.proc < rhs.proc;
}

#ifdef KATANA_USE_NUMA
int (*dynamic_numa_available)() = nullptr;
int (*dynamic_numa_num_configured_nodes)() = nullptr;
int (*dynamic_numa_node_of_cpu)(int cpu) = nullptr;

void
LoadLibNuma() {
  // KATANA_LIBNUMA_SO_NAME is defined in libgalois/CMakeLists.txt
  auto* lib = dlopen(KATANA_LIBNUMA_SO_NAME, RTLD_LAZY);
  if (!lib) {
    return;
  }
#define LOAD_SYM(name)                                                         \
  dynamic_##name = reinterpret_cast<decltype(&name)>(dlsym(lib, #name))
  LOAD_SYM(numa_available);
  LOAD_SYM(numa_num_configured_nodes);
  LOAD_SYM(numa_node_of_cpu);
#undef LOAD_SYM
}
#endif

unsigned
getNumaNode(cpuinfo& c) {
  static bool warnOnce = false;
#ifdef KATANA_USE_NUMA
  static bool numaAvail = false;

  if (!warnOnce) {
    warnOnce = true;
    LoadLibNuma();
    numaAvail = dynamic_numa_available && dynamic_numa_available() >= 0;
    numaAvail = numaAvail && dynamic_numa_num_configured_nodes() > 0;
    if (!numaAvail)
      katana::gWarn(
          "Numa support configured but not present at runtime.  "
          "Assuming numa topology matches socket topology.");
  }

  if (!numaAvail)
    return c.physid;
  int i = dynamic_numa_node_of_cpu(c.proc);
  if (i < 0)
    KATANA_SYS_DIE("failed finding numa node for ", c.proc);
  return i;
#else
  if (!warnOnce) {
    warnOnce = true;
    katana::gWarn(
        "Numa Support Not configured (install libnuma-dev).  "
        "Assuming numa topology matches socket topology.");
  }
  return c.physid;
#endif
}

//! Parse /proc/cpuinfo
std::vector<cpuinfo>
parseCPUInfo() {
  std::vector<cpuinfo> vals;

  std::string line;

  std::ifstream cpu_info("/proc/cpuinfo");
  if (!cpu_info) {
    KATANA_SYS_DIE("failed opening /proc/cpuinfo");
  }

  int cur = -1;

  while (std::getline(cpu_info, line)) {
    int num{};
    if (sscanf(line.data(), "processor : %d", &num) == 1) {
      KATANA_LOG_DEBUG_ASSERT(cur < num);
      cur = num;
      vals.resize(cur + 1);
      vals.at(cur).proc = num;
    } else if (sscanf(line.data(), "physical id : %d", &num) == 1) {
      vals.at(cur).physid = num;
    } else if (sscanf(line.data(), "siblings : %d", &num) == 1) {
      vals.at(cur).sib = num;
    } else if (sscanf(line.data(), "core id : %d", &num) == 1) {
      vals.at(cur).coreid = num;
    } else if (sscanf(line.data(), "cpu cores : %d", &num) == 1) {
      vals.at(cur).cpucores = num;
    }
  }

  for (auto& c : vals) {
    c.numaNode = getNumaNode(c);
  }

  return vals;
}

unsigned
countSockets(const std::vector<cpuinfo>& info) {
  std::set<unsigned> pkgs;
  for (auto& c : info)
    pkgs.insert(c.physid);
  return pkgs.size();
}

unsigned
countCores(const std::vector<cpuinfo>& info) {
  std::set<std::pair<int, int>> cores;
  for (auto& c : info)
    cores.insert(std::make_pair(c.physid, c.coreid));
  return cores.size();
}

unsigned
countNumaNodes(const std::vector<cpuinfo>& info) {
  std::set<unsigned> nodes;
  for (auto& c : info)
    nodes.insert(c.numaNode);
  return nodes.size();
}

void
markSMT(std::vector<cpuinfo>& info) {
  for (unsigned int i = 1; i < info.size(); ++i)
    if (info[i - 1].physid == info[i].physid &&
        info[i - 1].coreid == info[i].coreid)
      info[i].smt = true;
    else
      info[i].smt = false;
}

std::vector<int>
parseCPUSet() {
  std::vector<int> vals;

  std::ifstream data("/proc/self/status");

  if (!data) {
    return vals;
  }

  std::string line;
  std::string prefix("Cpus_allowed_list:");
  bool found = false;
  while (true) {
    std::getline(data, line);
    if (!data) {
      return vals;
    }

    if (line.compare(0, prefix.size(), prefix) == 0) {
      found = true;
      break;
    }
  }

  if (!found) {
    return vals;
  }

  line = line.substr(prefix.size());

  return katana::parseCPUList(line);
}

void
markValid(std::vector<cpuinfo>& info) {
  auto v = parseCPUSet();
  if (v.empty()) {
    for (auto& c : info)
      c.valid = true;
  } else {
    std::sort(v.begin(), v.end());
    for (auto& c : info)
      c.valid = std::binary_search(v.begin(), v.end(), c.proc);
  }
}

katana::HWTopoInfo
makeHWTopo() {
  katana::MachineTopoInfo retMTI;

  auto info = parseCPUInfo();
  std::sort(info.begin(), info.end());
  markSMT(info);
  markValid(info);

  info.erase(
      std::partition(
          info.begin(), info.end(), [](const cpuinfo& c) { return c.valid; }),
      info.end());

  std::sort(info.begin(), info.end());
  markSMT(info);
  retMTI.maxSockets = countSockets(info);
  retMTI.maxThreads = info.size();
  retMTI.maxCores = countCores(info);
  retMTI.maxNumaNodes = countNumaNodes(info);

  std::vector<katana::ThreadTopoInfo> retTTI;
  retTTI.reserve(retMTI.maxThreads);
  // compute renumberings
  std::set<unsigned> sockets;
  std::set<unsigned> numaNodes;
  for (auto& i : info) {
    sockets.insert(i.physid);
    numaNodes.insert(i.numaNode);
  }
  unsigned mid = 0;  // max socket id
  for (unsigned i = 0; i < info.size(); ++i) {
    unsigned pid = info[i].physid;
    unsigned repid =
        std::distance(sockets.begin(), sockets.find(info[i].physid));
    mid = std::max(mid, repid);
    unsigned leader = std::distance(
        info.begin(),
        std::find_if(info.begin(), info.end(), [pid](const cpuinfo& c) {
          return c.physid == pid;
        }));
    retTTI.push_back(katana::ThreadTopoInfo{
        i, leader, repid,
        (unsigned)std::distance(
            numaNodes.begin(), numaNodes.find(info[i].numaNode)),
        mid, info[i].proc, info[i].numaNode});
  }

  return {
      .machineTopoInfo = retMTI,
      .threadTopoInfo = retTTI,
  };
}

}  // namespace

katana::HWTopoInfo
katana::getHWTopo() {
  static SimpleLock lock;
  static std::unique_ptr<HWTopoInfo> data;

  std::lock_guard<SimpleLock> guard(lock);
  if (!data) {
    data = std::make_unique<HWTopoInfo>(makeHWTopo());
  }
  return *data;
}

//! binds current thread to OS HW context "proc"
bool
katana::bindThreadSelf([[maybe_unused]] unsigned osContext) {
#ifdef KATANA_USE_SCHED_SETAFFINITY
  cpu_set_t mask;
  /* CPU_ZERO initializes all the bits in the mask to zero. */
  CPU_ZERO(&mask);

  /* CPU_SET sets only the bit corresponding to cpu. */
  // void to cancel unused result warning
  (void)CPU_SET(osContext, &mask);

  /* sched_setaffinity returns 0 in success */
  if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
    katana::gWarn(
        "Could not set CPU affinity to ", osContext, "(", strerror(errno), ")");
    return false;
  }
  return true;
#else
  KATANA_WARN_ONCE(
      "Cannot set cpu affinity on this platform.  Performance will be bad.");
  return false;
#endif
}
