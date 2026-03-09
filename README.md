# OpenShift HPC Resource Management & gRPC Tuning POC
### Overview: The "Container CPU Illusion"
This repository contains a Proof of Concept (POC) designed to demonstrate and resolve severe performance degradation and Out-of-Memory (OOM) fragmentation for C++ High-Performance Computing (HPC) applications running on OpenShift.

The core of the problem is the "Container CPU Illusion." In a pre-Kubernetes environment, applications relied on standard OS interfaces like `std::thread::hardware_concurrency()` or `sysconf(_SC_NPROCESSORS_ONLN)` to dictate thread pool sizes. Inside a container, these functions pierce the container abstraction and return the physical host's total core count rather than the pod's `cgroup` limits.

On modern, dense OpenShift nodes, this illusion causes massive thread hyper-inflation. An application limited to 2 CPUs might detect 64 physical cores and spawn hundreds of threads. This leads to catastrophic Completely Fair Scheduler (CFS) throttling, severe L3 cache invalidation, and OOM crashes driven by memory arena fragmentation.

This POC addresses the issue at two levels:

1. **Application Layer (Primary):** A C++ drop-in solution that accurately parses Linux `cgroup` v1/v2 boundaries to safely scale thread pools, combined with an advanced hooking technique for legacy libraries.

2. **Infrastructure Layer (Secondary):** OpenShift cluster tuning using the `static` CPU Manager policy and NUMA alignment to achieve absolute bare-metal performance.  


#### Project Structure:

```
├── deployment.yaml                   # Kubernetes deployment configuring Guaranteed QoS and glibc limits
├── Dockerfile                        # Multi-stage build for the POC and offline stress-ng installation
├── epel-release-latest-9.noarch.rpm  # EPEL repository for UBI 9
├── hpc-kubeletconfig.yaml            # Kubelet configuration to enforce static CPU manager policy
├── hpc-mcp.yaml                      # Custom MachineConfigPool for HPC worker isolation
├── hpc-profile.yaml                  # PerformanceProfile for CPU reservation, isolation, and NUMA alignment
├── Judy-1.0.5-28.fc36.x86_64.rpm     # stress-ng dependency (Fedora 36)
├── hpc_app                           # Compiled C++ binary (entrypoint)
├── poc.cpp                           # C++ source code with cgroup parsing
├── hpc_adv_app                       # Compiled C++ binary (may run from pod)
├── pocadv.cpp                        # C++ source code with cgroup parsing and sysconf hooking 
└── stress-ng-0.14.02-1.fc36.x86_64.rpm # stress-ng binary for UBI 9 / RHEL 9 (Fedora 36)
```



## Application Layer Solution: C++ Container-Awareness

### **Core Case: Cgroup solution**
The primary solution to the CPU illusion is to replace naive hardware detection with a container-aware algorithm. The provided **[poc.cpp](/poc.cpp)** directly interrogates the Linux `cgroup` pseudo-filesystem to mathematically deduce the exact CPU limits imposed by OpenShift.

The code uses a multi-tiered heuristic approach:

1. **Cgroup v2:** It attempts to read `/sys/fs/cgroup/cpu.max`, dividing the `cfs_quota_us` by the `cfs_period_us` to get the exact core limit.

2. **Cgroup v1 (Fallback):** If v2 is absent, it reads `/sys/fs/cgroup/cpu/cpu.cfs_quota_us` and `/sys/fs/cgroup/cpu/cpu.cfs_period_us`.

3. **OS Affinity (Fallback):** It checks `sched_getaffinity` to see if the container runtime has actively pinned the process to specific cores.

Code Highlight: Core Detection Logic:
```c
// Tier 1a: Parse cgroup v2 cpu.max for CFS Quotas
std::ifstream cpu_max_file("/sys/fs/cgroup/cpu.max");
if (cpu_max_file.is_open()) {
    std::string quota_str, period_str;
    cpu_max_file >> quota_str >> period_str;
    if (quota_str!= "max" &&!quota_str.empty()) {
        long long quota = std::stoll(quota_str);
        long long period = std::stoll(period_str);
        if (period > 0) return static_cast<int>(std::ceil(static_cast<double>(quota) / period));
    }
}
```
**Compilation command:**
```bash
g++ -std=c++11 -O3 poc.cpp -o hpc_app
```
------

### **Advanced Case: Intercepting Legacy Libraries (gRPC)**  
While updating your own thread pool logic is straightforward, many C++ applications rely on third-party libraries that suffer from the same CPU illusion. A prominent example is older versions of the Google gRPC C++ library (e.g., v1.10.1).

As thoroughly documented by the open-source community, gRPC dynamically scales its `gpr_executor` using the host's physical core count, by following rule: `2 * sysconf(_SC_NPROCESSORS_ONLN)`. In OpenShift, this results in a sudden explosion of of hundreds(or even thousands) of executor threads, causing heavy lock contention and a death spiral of latency.    
**Note:** Issue [#7747](https://github.com/grpc/grpc/issues/7747) and Issue [#16557](https://github.com/grpc/grpc/issues/16557)


For production systems where **upgrading these older libraries is not possible**, the POC implements a highly effective workaround: **C++ Symbol Interposition**.

By globally overriding the `sysconf` function using `dlsym(RTLD_NEXT, "sysconf")`, we intercept the hardware query before it reaches the OS, feeding the legacy library the dynamically parsed `cgroup` limit instead.

**Code Highlight: The `sysconf` Override**
```c
#include <dlfcn.h>
#include <unistd.h>

// Intercept sysconf queries globally
extern "C" {
    long sysconf(int name) {
        if (name == _SC_NPROCESSORS_ONLN) {
            static int cached_cpu_limit = -1;
            static bool initialized = false;

            // Parse /sys/fs/cgroup/cpu.max exactly once to save I/O overhead
            if (!initialized) {
                cached_cpu_limit = detect_container_cpu_limit(); 
                initialized = true;
            }

            // Trick gRPC/legacy libraries into seeing only the pod's limit
            if (cached_cpu_limit > 0) {
                return cached_cpu_limit;
            }
        }
        
        // Fallback to the real libc sysconf for all other queries
        static long (*real_sysconf)(int) = nullptr;
        if (!real_sysconf) {
            real_sysconf = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
        }
        return real_sysconf(name);
    }
}
```

1. **C++ Compilation and gRPC Hooking Requirements**    
The **[pocadv.cpp](/pocadv.cpp)** file contains a custom extern "C" { long sysconf(int name) } block. This leverages `dlsym(RTLD_NEXT)` to intercept hardware queries made by the gRPC library, feeding it the detected OpenShift cgroup limit instead.

    **Crucial Linking Rules:**

    * You **must** compile this dynamically. Fully static binaries (`-static`) do not invoke the dynamic linker at runtime, which will cause `RTLD_NEXT` to fail and crash the application.

    * You must link the dynamic linking library (`-ldl`).

    **Compilation Command:**    
    ```bash
    g++ -std=c++11 -O3 poc.cpp -o poc -ldl -pthread
    ```

    *(If using CMake, ensure `${CMAKE_DL_LIBS}` is added to your `target_link_libraries`).*

## 2. Containerization (Dockerfile)

The included `Dockerfile` utilizes a Red Hat Universal Base Image (UBI 9). It bypasses subscription requirements by installing the necessary `stress-ng` RPMs and EPEL repositories locally from the project tree.

**Build and Push:** 
```bash
podman build -t docker.io/<your-username>/hpc-poc:v1.
podman push docker.io/<your-username>/hpc-poc:v1
```

## 3. Native Thread Diagnostics (Without perf)
If advanced profiling tools like `perf` are unavailable inside the container, you can extract native thread behavior, CPU affinity, and scheduler wait times directly from the Linux `/proc` filesystem.

Exec into your running pod (`oc exec -it <pod-name> -- /bin/bash`) and run the following bash loop to generate a clean thread diagnostic report:

```bash
for tid in /proc/self/task/*; do 
    name=$(awk '/^Name:/ {print $2}' "$tid/status" 2>/dev/null)
    state=$(awk '/^State:/ {print $2, $3}' "$tid/status" 2>/dev/null)
    runtime=$(awk '/se\.sum_exec_runtime/ {print $3}' "$tid/sched" 2>/dev/null)
    
    if [ -n "$name" ]; then
        echo "TID: $(basename $tid) | Name: $name | State: $state | CPU Time: $runtime"
    fi
done
```

Note: Use `grep "^Cpus_allowed_list:" /proc/self/task/*/status` to verify if OpenShift has successfully pinned the threads to specific cores.


## 4. Benchmarking Phase A: Untuned Baseline  

#### Understanding Node Resources and the "12 CPU" Limit

In the deployment examples below, the pod is configured to request exactly `12 CPUs`. This number is not arbitrary. When configuring OpenShift for High-Performance Computing, you cannot allocate 100% of a node's physical cores to a workload.

To provide reliable scheduling and prevent node resource overcommitment, OpenShift subtracts a reserved portion of CPU and memory from the node's total capacity to calculate the "Allocatable" resources. These reservations (`system-reserved` and `kube-reserved`) ensure that the operating system, `kubelet`, CRI-O runtime, and network routing remain stable under heavy load.

For instance, on a modern node with **16** physical cores, reserving **4** cores for the platform overhead leaves exactly **12** allocatable cores for user workloads. Requesting 12 CPUs ensures the pod fits perfectly within the node's boundaries without starving the OpenShift control plane.     

**Note:** For detailed calculations, refer to the [Allocating resources for nodes in an OpenShift Container Platform cluster](https://docs.redhat.com/en/documentation/openshift_container_platform/4.18/html/nodes/working-with-nodes#nodes-nodes-resources-configuring).

First, we establish a performance baseline on standard worker nodes where threads float across all available cores, inducing context-switching overhead.   

1. Label the target nodes:

    ```bash
    oc label node <worker-node-1> hpc-dedicated=true
    oc label node <worker-node-2> hpc-dedicated=true
    ```

2. Deploy the application:  
Ensure `deployment.yaml` sets both CPU requests and limits to `12`.

    ```bash
    oc apply -f deployment.yaml
    ```

3. Execute the Baseline Test:   
Exec into the pod, ensure you are in the `/tmp` directory, and run the matrix math stressor:

    ```bash
    oc exec -it <pod-name> -- /bin/bash
    cd /tmp
    stress-ng --cpu 12 --cpu-method matrixprod --timeout 60s --metrics-brief
    ```
4. Record the Results:  
Note the `bogo ops/s` output. This represents the application's throughput under standard CFS scheduling.

## 5. Benchmarking Phase B: OpenShift Hardware Tuning
We will now use the Node Tuning Operator to isolate the CPUs, apply the `static` CPU manager policy, and enforce NUMA alignment.    

**Note:** For detailed calculations, refer to the following resources:         
    - [Allocating specific CPUs for nodes in a cluster](https://docs.redhat.com/en/documentation/openshift_container_platform/4.18/html/nodes/working-with-nodes#nodes-nodes-resources-cpus)       
    - [Using CPU Manager and Topology Manager](https://docs.redhat.com/en/documentation/openshift_container_platform/4.18/html/scalability_and_performance/using-cpu-manager)       
    - [Scheduling NUMA-aware workloads](https://docs.redhat.com/en/documentation/openshift_container_platform/4.18/html/scalability_and_performance/cnf-numa-aware-scheduling)      
    - [Sample Performance Profile](https://docs.redhat.com/en/documentation/openshift_container_platform/4.18/html/scalability_and_performance/cnf-numa-aware-scheduling#cnf-sample-performance-policy_numa-aware)      

------

1. Assign the HPC role to the dedicated nodes:

    ```bash
    oc label node -l hpc-dedicated=true node-role.kubernetes.io/hpc=""
    ```

2. Apply the Custom Machine Config Pool (MCP):

    ```bash
    oc apply -f hpc-mcp.yaml
    ```

3. Apply the Performance Profile & Kubelet Config:
This reserves cores `0-3` for the OS/Master processes and isolates the remaining cores for the HPC workload.

    ```bash
    oc apply -f hpc-profile.yaml
    oc apply -f hpc-kubeletconfig.yaml
    ```

4. Wait for Reboots:    
The Machine Config Operator will drain and reboot the nodes. Wait until the `hpc` pool is fully updated:

    ```bash
    oc get mcp hpc -w
    ```

## 6. Benchmarking Phase C: Optimized Results
Once the nodes reboot, OpenShift will recognize the pod's `Guaranteed` QoS class (requests == limits = 12) and permanently pin the container to 12 isolated hardware cores.

1. Verify the Hardware Lock:    
Check the application logs to see the output from our C++ `sysconf` override probe:

    ```bash
    oc logs deployment/hpc-poc
    ```

    It should successfully report `OpenShift/K8s 'static' policy (Exclusive Pinned Cores)` and an affinity mask of exactly `12`.

2. Execute the Final Benchmark:     
Exec into the pod again and run the exact same test:

    ```bash
    oc exec -it <pod-name> -- /bin/bash
    cd /tmp
    stress-ng --cpu 12 --cpu-method matrixprod --timeout 60s --metrics-brief
    ```

3. Compare Results:     
Compare the new `bogo ops/s` metric against the untuned baseline. The throughput will be significantly higher due to the elimination of CPU cache invalidation, context-switching, and CFS throttling.