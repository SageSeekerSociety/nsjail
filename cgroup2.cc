/*

   nsjail - cgroup2 namespacing
   -----------------------------------------

   Copyright 2014 Google Inc. All Rights Reserved.

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

#include "cgroup2.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include "logs.h"
#include "util.h"

namespace cgroup2 {

static bool addPidToProcList(const std::string &cgroup_path, pid_t pid) {
	std::string pid_str = std::to_string(pid);

	LOG_D("Adding pid='%s' to cgroup.procs", pid_str.c_str());
	if (!util::writeBufToFile((cgroup_path + "/cgroup.procs").c_str(), pid_str.c_str(),
		pid_str.length(), O_WRONLY)) {
		LOG_W("Could not update cgroup.procs");
		return false;
	}
	return true;
}

static std::string getCgroupPath(nsjconf_t *nsjconf, pid_t pid) {
	return nsjconf->cgroupv2_mount + "/NSJAIL." + std::to_string(pid);
}
static std::string getJailCgroupPath(nsjconf_t *nsjconf) {
	return nsjconf->cgroupv2_mount + "/NSJAIL_SELF." + std::to_string(getpid());
}

static bool createCgroup(const std::string &cgroup_path, pid_t pid) {
	LOG_D("Create '%s' for pid=%d", cgroup_path.c_str(), (int)pid);
	if (mkdir(cgroup_path.c_str(), 0700) == -1 && errno != EEXIST) {
		PLOG_W("mkdir('%s', 0700) failed", cgroup_path.c_str());
		return false;
	}
	return true;
}

static bool moveSelfIntoChildCgroup(nsjconf_t *nsjconf) {
	/*
	 * Move ourselves into another group to avoid the 'No internal processes' rule
	 * https://unix.stackexchange.com/a/713343
	 */
	std::string jail_cgroup_path = getJailCgroupPath(nsjconf);
	LOG_I("nsjail is moving itself to a new child cgroup: %s\n", jail_cgroup_path.c_str());
	RETURN_ON_FAILURE(createCgroup(jail_cgroup_path, getpid()));
	RETURN_ON_FAILURE(addPidToProcList(jail_cgroup_path, 0));
	return true;
}

static bool enableCgroupSubtree(nsjconf_t *nsjconf, const std::string &controller, pid_t pid) {
	std::string cgroup_path = nsjconf->cgroupv2_mount;
	LOG_D("Enable cgroup.subtree_control +'%s' to '%s' for pid=%d", controller.c_str(),
	    cgroup_path.c_str(), pid);
	std::string val = "+" + controller;

	/*
	 * Try once without moving the nsjail process and if that fails then try moving the nsjail
	 * process into a child cgroup before trying a second time.
	 */
	if (util::writeBufToFile((cgroup_path + "/cgroup.subtree_control").c_str(), val.c_str(),
		val.length(), O_WRONLY, false)) {
		return true;
	}
	if (errno == EBUSY) {
		RETURN_ON_FAILURE(moveSelfIntoChildCgroup(nsjconf));
		if (util::writeBufToFile((cgroup_path + "/cgroup.subtree_control").c_str(),
			val.c_str(), val.length(), O_WRONLY)) {
			return true;
		}
	}
	LOG_E(
	    "Could not apply '%s' to cgroup.subtree_control in '%s'. nsjail MUST be run from root "
	    "and the cgroup mount path must refer to the root/host cgroup to use cgroupv2. If you "
	    "use Docker, you may need to run the container with --cgroupns=host so that nsjail can"
	    " access the host/root cgroupv2 hierarchy. An alternative is mounting (or remounting) "
	    "the cgroupv2 filesystem but using the flag is just simpler.",
	    val.c_str(), cgroup_path.c_str());
	return false;
}

static bool writeToCgroup(
    const std::string &cgroup_path, const std::string &resource, const std::string &value) {
	LOG_I("Setting '%s' to '%s'", resource.c_str(), value.c_str());

	if (!util::writeBufToFile(
		(cgroup_path + "/" + resource).c_str(), value.c_str(), value.length(), O_WRONLY)) {
		LOG_W("Could not update %s", resource.c_str());
		return false;
	}
	return true;
}

static void removeCgroup(const std::string &cgroup_path) {
	long long memory_peak_bytes = -1;
	long long user_usec = -1;
	long long system_usec = -1;
	long long total_cpu_usec = -1;

	// 1. Read memory.peak using C stdio
	std::string mem_peak_path_str = cgroup_path + "/memory.peak";
	const char *mem_peak_path = mem_peak_path_str.c_str();
	FILE *fp_mem = fopen(mem_peak_path, "r");
	if (fp_mem != NULL) {
		char mem_buf[64];  // Buffer to read the number string
		if (fgets(mem_buf, sizeof(mem_buf), fp_mem) != NULL) {
			char *endptr = NULL;
			errno = 0;  // Reset errno before strtoll
			long long val = strtoll(mem_buf, &endptr, 10);

			// Check for conversion errors
			if (errno == ERANGE) {
				PLOG_W(
				    "Value in '%s' is out of range for long long", mem_peak_path);
				memory_peak_bytes = -1;
			} else if (errno != 0 && val == 0) {
				PLOG_W("Error converting value in '%s'", mem_peak_path);
				memory_peak_bytes = -1;
			} else if (endptr == mem_buf) {
				LOG_W("No numerical digits found in '%s'. Content starting with: "
				      "'%.10s'",
				    mem_peak_path, mem_buf);
				memory_peak_bytes = -1;
			} else {
				char *check_ptr = endptr;
				while (*check_ptr != '\0' && isspace((unsigned char)*check_ptr)) {
					check_ptr++;
				}
				if (*check_ptr != '\0') {
					LOG_W("Extra non-numeric/non-whitespace characters found "
					      "after number in '%s'. Content: '%.20s'",
					    mem_peak_path, mem_buf);
					memory_peak_bytes = -1;
				} else if (val < 0) {
					LOG_W("Parsed negative memory peak value from '%s': %lld",
					    mem_peak_path, val);
					memory_peak_bytes = -1;
				} else {
					memory_peak_bytes = val;  // Success
				}
			}
		} else {
			if (feof(fp_mem)) {
				LOG_W("File '%s' is empty.", mem_peak_path);
			} else {  // ferror(fp_mem) must be true
				PLOG_W("Error reading from file '%s'", mem_peak_path);
			}
			memory_peak_bytes = -1;
		}
		fclose(fp_mem);
	} else {
		if (errno == ENOENT) {
			LOG_D("File '%s' not found (errno=%d). Cgroup might have been removed.",
			    mem_peak_path, errno);
		} else {
			PLOG_W("Failed to open file '%s'", mem_peak_path);
		}
		// memory_peak_bytes remains -1
	}

	// 2. Read cpu.stat using C stdio
	std::string cpu_stat_path_str = cgroup_path + "/cpu.stat";
	const char *cpu_stat_path = cpu_stat_path_str.c_str();
	FILE *fp_cpu = fopen(cpu_stat_path, "r");
	if (fp_cpu != NULL) {
		char line[256];
		while (fgets(line, sizeof(line), fp_cpu) != NULL) {
			if (user_usec == -1 && strncmp(line, "user_usec ", 10) ==
						   0) {	 // Process only if not already found
				char *value_ptr = line + 10;
				char *endptr = NULL;
				errno = 0;
				long long val = strtoll(value_ptr, &endptr, 10);

				if (errno == ERANGE) {
					PLOG_W("user_usec value out of range in '%s'. Line: '%s'",
					    cpu_stat_path, line);
					user_usec = -2;
				}  // Use -2 to distinguish parse error from not found? Or just keep
				   // -1.
				else if (errno != 0 && val == 0) {
					PLOG_W("Error converting user_usec in '%s'. Line: '%s'",
					    cpu_stat_path, line);
					user_usec = -1;
				} else if (endptr == value_ptr) {
					LOG_W("No numerical digits found for user_usec in '%s'. "
					      "Line: '%s'",
					    cpu_stat_path, line);
					user_usec = -1;
				} else {
					char *check_ptr = endptr;
					while (*check_ptr != '\0' &&
					       isspace((unsigned char)*check_ptr))
						check_ptr++;
					if (*check_ptr != '\0') {
						LOG_W("Extra chars after user_usec value in '%s'. "
						      "Line: '%s'",
						    cpu_stat_path, line);
						user_usec = -1;
					} else if (val < 0) {
						LOG_W("Parsed negative user_usec value from '%s': "
						      "%lld",
						    cpu_stat_path, val);
						user_usec = -1;
					} else {
						user_usec = val;
					}  // Success
				}
			} else if (system_usec == -1 &&
				   strncmp(line, "system_usec ", 12) ==
				       0) {  // Process only if not already found
				char *value_ptr = line + 12;
				char *endptr = NULL;
				errno = 0;
				long long val = strtoll(value_ptr, &endptr, 10);

				if (errno == ERANGE) {
					PLOG_W("system_usec value out of range in '%s'. Line: '%s'",
					    cpu_stat_path, line);
					system_usec = -1;
				} else if (errno != 0 && val == 0) {
					PLOG_W("Error converting system_usec in '%s'. Line: '%s'",
					    cpu_stat_path, line);
					system_usec = -1;
				} else if (endptr == value_ptr) {
					LOG_W("No numerical digits for system_usec in '%s'. Line: "
					      "'%s'",
					    cpu_stat_path, line);
					system_usec = -1;
				} else {
					char *check_ptr = endptr;
					while (*check_ptr != '\0' &&
					       isspace((unsigned char)*check_ptr))
						check_ptr++;
					if (*check_ptr != '\0') {
						LOG_W("Extra chars after system_usec value in "
						      "'%s'. Line: '%s'",
						    cpu_stat_path, line);
						system_usec = -1;
					} else if (val < 0) {
						LOG_W("Parsed negative system_usec value from "
						      "'%s': %lld",
						    cpu_stat_path, val);
						system_usec = -1;
					} else {
						system_usec = val;
					}  // Success
				}
			}

			// Optimization: if both valid values found, stop reading
			if (user_usec != -1 && system_usec != -1) {
				break;
			}
		}  // end while fgets

		if (ferror(fp_cpu)) {  // Check if loop terminated due to read error
			PLOG_W("Error occurred while reading '%s'", cpu_stat_path);
			// If an error occurred mid-read, already found values might be suspect?
			// For simplicity, keep them, but total will likely remain -1 if one is
			// missing.
		}
		fclose(fp_cpu);

		// Calculate total only if both components were successfully parsed and are
		// non-negative
		if (user_usec >= 0 && system_usec >= 0) {
			total_cpu_usec = user_usec + system_usec;
		} else {
			// Log if we couldn't get both valid CPU times (and at least one wasn't just
			// missing due to early exit)
			if (user_usec == -1 || system_usec == -1) {
				LOG_W("Could not determine total CPU usage from '%s' "
				      "(user_usec=%lld, system_usec=%lld)",
				    cpu_stat_path, user_usec, system_usec);
			}
			total_cpu_usec =
			    -1;	 // Ensure total is -1 if components are invalid or missing
		}

	} else {
		// fopen failed for cpu.stat
		if (errno == ENOENT) {
			LOG_D("File '%s' not found (errno=%d). Cgroup might have been removed.",
			    cpu_stat_path, errno);
		} else {
			PLOG_W("Failed to open file '%s'", cpu_stat_path);
		}
		// Values remain -1
	}

	// 3. Log the collected statistics using nsjail's logging
	//    Use LOG_I for informational, or LOG_D for debug level.
	LOG_I("Cgroup Stats: CPU_usec=%lld MEM_peak_bytes=%lld (user=%lld, system=%lld)",
	    total_cpu_usec, memory_peak_bytes, user_usec, system_usec);

	LOG_D("Remove '%s'", cgroup_path.c_str());
	if (rmdir(cgroup_path.c_str()) == -1) {
		PLOG_W("rmdir('%s') failed", cgroup_path.c_str());
	}
}

static bool needMemoryController(nsjconf_t *nsjconf) {
	/*
	 * Check if we need 'memory'
	 * This matches the check in initNsFromParentMem()
	 */
	ssize_t swap_max = nsjconf->cgroup_mem_swap_max;
	if (nsjconf->cgroup_mem_memsw_max > (size_t)0) {
		swap_max = nsjconf->cgroup_mem_memsw_max - nsjconf->cgroup_mem_max;
	}
	if (nsjconf->cgroup_mem_max == (size_t)0 && swap_max < (ssize_t)0) {
		return false;
	}
	return true;
}

static bool needPidsController(nsjconf_t *nsjconf) {
	return nsjconf->cgroup_pids_max != 0;
}

static bool needCpuController(nsjconf_t *nsjconf) {
	return nsjconf->cgroup_cpu_ms_per_sec != 0U;
}

/*
 * We will use this buf to read from cgroup.subtree_control to see if
 * the root cgroup has the necessary controllers listed
 */
#define SUBTREE_CONTROL_BUF_LEN 0x40

bool setup(nsjconf_t *nsjconf) {
	/*
	 * Read from cgroup.subtree_control in the root to see if
	 * the controllers we need are there.
	 */
	auto p = nsjconf->cgroupv2_mount + "/cgroup.subtree_control";
	char buf[SUBTREE_CONTROL_BUF_LEN];
	int read = util::readFromFile(p.c_str(), buf, SUBTREE_CONTROL_BUF_LEN - 1);
	if (read < 0) {
		LOG_W("cgroupv2 setup: Could not read root subtree_control");
		return false;
	}
	buf[read] = 0;

	/* Are the controllers we need there? */
	bool subtree_ok = (!needMemoryController(nsjconf) || strstr(buf, "memory")) &&
			  (!needPidsController(nsjconf) || strstr(buf, "pids")) &&
			  (!needCpuController(nsjconf) || strstr(buf, "cpu"));
	if (!subtree_ok) {
		/* Now we can write to the root cgroup.subtree_control */
		if (needMemoryController(nsjconf)) {
			RETURN_ON_FAILURE(enableCgroupSubtree(nsjconf, "memory", getpid()));
		}

		if (needPidsController(nsjconf)) {
			RETURN_ON_FAILURE(enableCgroupSubtree(nsjconf, "pids", getpid()));
		}

		if (needCpuController(nsjconf)) {
			RETURN_ON_FAILURE(enableCgroupSubtree(nsjconf, "cpu", getpid()));
		}
	}
	return true;
}

bool detectCgroupv2(nsjconf_t *nsjconf) {
	/*
	 * Check cgroupv2_mount, if it is a cgroup2 mount, use it.
	 */
	struct statfs buf;
	if (statfs(nsjconf->cgroupv2_mount.c_str(), &buf)) {
		LOG_D("statfs %s failed with %d", nsjconf->cgroupv2_mount.c_str(), errno);
		nsjconf->use_cgroupv2 = false;
		return false;
	}
	nsjconf->use_cgroupv2 = (buf.f_type == CGROUP2_SUPER_MAGIC);
	return true;
}

static bool initNsFromParentMem(nsjconf_t *nsjconf, pid_t pid) {
	ssize_t swap_max = nsjconf->cgroup_mem_swap_max;
	if (nsjconf->cgroup_mem_memsw_max > (size_t)0) {
		swap_max = nsjconf->cgroup_mem_memsw_max - nsjconf->cgroup_mem_max;
	}

	if (nsjconf->cgroup_mem_max == (size_t)0 && swap_max < (ssize_t)0) {
		return true;
	}

	std::string cgroup_path = getCgroupPath(nsjconf, pid);
	RETURN_ON_FAILURE(createCgroup(cgroup_path, pid));
	RETURN_ON_FAILURE(addPidToProcList(cgroup_path, pid));

	if (nsjconf->cgroup_mem_max > (size_t)0) {
		RETURN_ON_FAILURE(writeToCgroup(
		    cgroup_path, "memory.max", std::to_string(nsjconf->cgroup_mem_max)));
	}

	if (swap_max >= (ssize_t)0) {
		RETURN_ON_FAILURE(
		    writeToCgroup(cgroup_path, "memory.swap.max", std::to_string(swap_max)));
	}

	return true;
}

static bool initNsFromParentPids(nsjconf_t *nsjconf, pid_t pid) {
	if (nsjconf->cgroup_pids_max == 0U) {
		return true;
	}
	std::string cgroup_path = getCgroupPath(nsjconf, pid);
	RETURN_ON_FAILURE(createCgroup(cgroup_path, pid));
	RETURN_ON_FAILURE(addPidToProcList(cgroup_path, pid));
	return writeToCgroup(cgroup_path, "pids.max", std::to_string(nsjconf->cgroup_pids_max));
}

static bool initNsFromParentCpu(nsjconf_t *nsjconf, pid_t pid) {
	if (nsjconf->cgroup_cpu_ms_per_sec == 0U) {
		return true;
	}

	std::string cgroup_path = getCgroupPath(nsjconf, pid);
	RETURN_ON_FAILURE(createCgroup(cgroup_path, pid));
	RETURN_ON_FAILURE(addPidToProcList(cgroup_path, pid));

	/*
	 * The maximum bandwidth limit in the format: `$MAX $PERIOD`.
	 * This indicates that the group may consume up to $MAX in each $PERIOD
	 * duration.
	 */
	std::string cpu_ms_per_sec_str = std::to_string(nsjconf->cgroup_cpu_ms_per_sec * 1000U);
	cpu_ms_per_sec_str += " 1000000";
	return writeToCgroup(cgroup_path, "cpu.max", cpu_ms_per_sec_str);
}

bool initNsFromParent(nsjconf_t *nsjconf, pid_t pid) {
	RETURN_ON_FAILURE(initNsFromParentMem(nsjconf, pid));
	RETURN_ON_FAILURE(initNsFromParentPids(nsjconf, pid));
	return initNsFromParentCpu(nsjconf, pid);
}

void finishFromParent(nsjconf_t *nsjconf, pid_t pid) {
	if (nsjconf->cgroup_mem_max != (size_t)0 || nsjconf->cgroup_pids_max != 0U ||
	    nsjconf->cgroup_cpu_ms_per_sec != 0U) {
		removeCgroup(getCgroupPath(nsjconf, pid));
	}
}

}  // namespace cgroup2
