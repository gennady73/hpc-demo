# **OpenShift CPU Pinning & Dedicated Node Performance Report**

## **Executive Summary**

This document complements the [OpenShift HPC Resource Management & gRPC Tuning POC](/README.md) document by outlining the performance impact of utilizing OpenShift's static CPU Manager policy and dedicated nodes versus the default Linux Completely Fair Scheduler (CFS) for heavy, multi-threaded workloads.

Based on rigorous A/B testing, enforcing strict CPU isolation yielded massive performance gains without upgrading any physical hardware:

* **CPU Math Operations:** 167% faster.  
* **Thread Creation:** 226% faster.  
* **Kernel Overhead:** Reduced from \~12% to \~3%.

## **1. The Experiment Topology (A/B Test)**

To prove the value of CPU pinning, we deployed an identical heavy application profile across two distinct OpenShift node configurations. Each pod was allocated 6 CPUs and 2Gi of Memory. The physical worker nodes are equipped with 32 cores.

* **Group A (Dedicated HPC Nodes):** Utilizing a custom `KubeletConfig` with `cpuManagerPolicy: static`. The pods were granted Guaranteed QoS, locking them to 6 dedicated physical cores (via `cpuset`). The node(s) label is `hpc-dedicated: "true"` and roles `hpc, worker`. The role hpc will be used by `machineConfigSelector`.  Uses [deployment.yaml](/deployment.yaml).  
* **Group B (Standard CFS Node):** Utilizing the default OpenShift scheduler. The pods competed for CPU time slices across all 32 physical cores on the node. The node(s) label is `hpc-common: "true"` and role `worker` which is a default. Uses [deployment-cfs.yaml](/deployment-cfs.yaml).

:![OpenShift node groups](/docs/resources/ocp-node-groups.png)

## **2. Workload Execution & Observability**

### **Running the Workloads via OpenShift CLI**

To execute the workloads inside the running pods, the following standard `oc exec` commands were utilized:

```bash
# Execute the standard stress-ng test
oc exec -it <pod_name> -n hpc-test -- sh -c "cd /tmp && stress-ng --cpu 6 --vm 2 --vm-bytes 512M --io 1 --hdd 1 --hdd-bytes 1G --pthread 2 --timeout 2m --metrics-brief"

# Execute the legacy application test
oc exec -it <pod_name> -n hpc-test -- ./hpc_app

# Execute the advanced/fixed application test
oc exec -it <pod_name> -n hpc-test -- ./hpc_adv_app
```

### **Observability Queries (PromQL)**

To capture the metrics and validate the performance screenshots in this report, the following queries were executed in the OpenShift metrics console:

**Note:** The `-vvdrh` is a node specific name in this particualr deployment, replace with actual name when using thiese queries on your own cluster.       

* **Node System Overhead (Kernel Thrashing):**

    ```js
    sum(irate(node_cpu_seconds_total{mode="system", instance=~"worker-cluster-vvdrh.*"}[5m])) by (instance) / sum(irate(node_cpu_seconds_total{instance=~"worker-cluster-vvdrh.*"}[5m])) by (instance) * 100
    ```  

* **Instant CPU Usage per Pod:**

    ```js
    sum(irate(container_cpu_usage_seconds_total{namespace="hpc-test", pod=~"hpc-poc-.*", container!="", container!="POD"}[5m])) by (pod)
    ```  

* **Disk I/O Wait (To rule out storage bottlenecks):**

    ```js
    sum(irate(node_cpu_seconds_total{mode="iowait", instance=~"worker-cluster-<worker-id>.*"}[5m])) by (instance) / sum(irate(node_cpu_seconds_total{instance=~"worker-cluster-<worker-id>.*"}[5m])) by (instance) * 100
    ```  

## **3. Application CPU Visibility & Code Fixes**

A major challenge in containerized HPC environments is that legacy applications often query the host's physical hardware rather than their cgroup limits. When an application queries `sysconf(_SC_NPROCESSORS_ONLN)`, it will often see the node's total 32 cores instead of the pod's 6-core limit.

We tested two versions of the application to demonstrate this visibility issue and the necessary code-level fixes (e.g., using `LD_PRELOAD` to intercept system calls or updating the app to read cgroup limits).

### **Standard Node (CFS Time-Slicing)**

On the standard node, the CFS quota is 6, but the affinity mask exposes all 32 cores.

**Standard App (`hpc_app`):** Sees all 32 cores.

```
================================================== 
HPC Application Runtime Environment Report
==================================================

Detected OS Environment  : Linux (Potential Container/OpenShift)
Hardware Concurrency     : 32 cores (Physical/Logical Host)
Detected Cgroup Version  : v2
Cgroup CFS Quota Limit   : 6 CPUs
Scheduler Affinity Mask  : 32 CPUs
Inferred CPU Manager     : OpenShift/K8s 'none' policy (CFS Time-Slicing / Floating Threads)
```

**Advanced App (`hpc_adv_app`):** Intercepts the system call to accurately return 6 cores.

```
>>> PROOF: sysconf(_SC_NPROCESSORS_ONLN) intercepted. Returning: 6 cores.
```

### **Dedicated Node (HPC Pinning)**

On the dedicated node, OpenShift uses `cpuset` to physically mask the CPUs. The CFS Quota is disabled entirely for maximum performance.

**Standard App (`hpc_app`):** The `cpuset` successfully restricts the affinity mask to 6, protecting the application even without a code fix.

```
================================================== 
HPC Application Runtime Environment Report
==================================================

Hardware Concurrency     : 32 cores (Physical/Logical Host)
Cgroup CFS Quota Limit   : Unlimited/Not Found CPUs
Scheduler Affinity Mask  : 6 CPUs
Inferred CPU Manager     : Unrestricted (No CFS Quota Detected)
```

**Advanced App (`hpc_adv_app`):** Correctly identifies the `static` policy and intercepts the core count.

```
Inferred CPU Manager     : OpenShift 'static' policy (CFS Quota Disabled for Peak Perf)
--------------------------------------------------
>>> ACTION: Initializing thread pool with 6 workers.
>>> PROOF: sysconf(_SC_NPROCESSORS_ONLN) intercepted. Returning: 6 cores.
``` 

## **4. The Penalty of Over-Commitment (Thrashing)**

If an application is not fixed (like the standard `hpc_app` above) and detects the host's 32 cores, it will attempt to spawn 32 active working threads, even though OpenShift has limited it to a 6-CPU quota. This leads to severe performance degradation due to **thread thrashing**.        

When a pod asks for significantly more active threads than its CPU limit can support, the Linux Completely Fair Scheduler (CFS) must constantly pause, swap out, and resume threads (Context Switching).

**Example via `stress-ng`:**

If a pod has a limit of 6 CPUs, running `--cpu 6` (a perfect fit) results in maximum throughput. Forcing the pod to over-commit by running `--cpu 32` (simulating an app that erroneously read the host's 32-core hardware size) results in actual performance loss:

* **6 Threads (Perfect Fit):** Threads map cleanly to the allocated quota. The kernel spends maximum time executing actual application code.  
* **32 Threads (Over-committed):** The 32 threads constantly fight for time-slices within the 6-core quota limits.

By over-allocating threads, the kernel spends a significant portion of its usable CPU time managing the **"traffic jam"** (pausing and resuming threads) rather than executing actual application logic, substantially lowering the total operations completed.

## **5. Results: Raw Processing Power**

By strictly isolating the pods on the HPC nodes and preventing context switching, the dedicated applications dramatically outperformed the standard CFS nodes during the 120-second test window.        

| **Metric** | **Group B: Shared Node (CFS)** | **Group A: Dedicated Node (CPU Pinning)** | **Performance Increase** |
|---|---|---|---|
| **Compute (cpu ops)** | \~150,880 | \~403,822 | **\+167%** (2.6x faster) |
| **Memory (vm ops)** | \~2,592,067 | \~3,334,283 | **\+28%** (1.2x faster) |
| **Threading (pthread ops)** | \~660,734 | \~2,156,650 | **\+226%** (3.2x faster) |


### **Example stress-ng Terminal Outputs**

**Standard Node (CFS):** Notice the lower bogo ops and lower user time (usr time) due to context switching.     
```
stress-ng: info:  [7] stressor       bogo ops real time  usr time  sys time   bogo ops/s     bogo ops/s  
stress-ng: info:  [7] cpu              150121    120.03    252.00    252.00      1250.68       59571.83  
stress-ng: info:  [7] vm              2586956    120.19     68.00     68.00     21523.32     3079709.52  
stress-ng: info:  [7] pthread          660988    120.08     20.00     20.00      5504.71      216717.38
```     
**Dedicated Node (HPC Pinning):** Notice the significantly higher bogo ops and user time in the exact same 120-second window.
```
stress-ng: info:  [4] stressor       bogo ops real time  usr time  sys time   bogo ops/s     bogo ops/s  
stress-ng: info:  [4] cpu              384210    120.00    336.00    336.00      3201.66      114008.90  
stress-ng: info:  [4] vm              3234923    120.02     84.00     84.00     26952.56     3516220.65  
stress-ng: info:  [4] pthread         2047820    120.02     26.00     26.00     17062.68     1383662.16
```


## **6. Analysis: The Cost of Context Switching**

The primary cause of Group B's poor performance was kernel overhead. Because the CFS scheduler randomly migrated threads across all 32 physical cores, the CPU spent massive amounts of time flushing L1/L2 caches and context-switching.

* **System Overhead (Standard Node):** Spiked to **10-12%** during the load test.  
* **System Overhead (HPC Nodes):** Remained stable at **3-5%**.  
* **Disk I/O Wait:** Ruled out as a bottleneck (peaked at `0.3%`).

Group A avoided the CPU traffic jam entirely. By using the `cpuset` feature via the static CPU manager, threads remained locked to their specific physical cores, keeping CPU caches hot and eliminating migration penalties.

### **Visualizing the Overhead (Step-by-Step Instructions)**

To reproduce the screenshots visualizing this context switching overhead, follow these steps in the OpenShift Web Console:

**Screenshot 1: System CPU Overhead**

1. Navigate to **Observe** \-\> **Metrics** in the OpenShift console.  
2. Paste the following PromQL query to visualize the percentage of CPU time spent in the system state (kernel overhead):  
   ```js
   sum(irate(node_cpu_seconds_total{mode="system", instance=~"worker-cluster-vvdrh.*"}[5m])) by (instance) / sum(irate(node_cpu_seconds_total{instance=~"worker-cluster-vvdrh.*"}[5m])) by (instance) * 100
   ```

3. Set the time window to match your load test duration (e.g., 5m).  
4. Capture the screenshot showing the CFS nodes spiking to 10-12% while HPC nodes remain at 3-5%.

![System CPU Overhead](/docs/resources/system-cpu-overhead.png)

**Screenshot 2: Instant CPU Usage per Pod**

1. In the same **Observe** \-\> **Metrics** view, clear the previous query.
2. Paste the following PromQL query to visualize the actual CPU utilization from the container's perspective:
    ```js
    sum(irate(container_cpu_usage_seconds_total{namespace="hpc-test", pod=~"hpc-poc-.*", container!="", container!="POD"}[5m])) by (pod)
    ```
![CPU Usage per Pod](/docs/resources/cpu-usage-per-pod.png)

**Screenshot 3: Disk I/O Wait (Verifying Storage is NOT the Bottleneck)**

1. In the same **Observe** \-\> **Metrics** view, clear the previous query.  
2. Paste the following PromQL query to ensure the system overhead is from context switching, not slow disks:  
   ```js
   sum(irate(node_cpu_seconds_total{mode="iowait", instance=~"worker-cluster-vvdrh.*"}[5m])) by (instance) / sum(irate(node_cpu_seconds_total{instance=~"worker-cluster-vvdrh.*"}[5m])) by (instance) * 100
   ```

3. Capture the screenshot showing values near \~0.3%, definitively proving that storage was not the bottleneck.

![Disk I/O Wait](/docs/resources/disk-io-wait.png)

## **7. Production Implementation Guidelines**

To achieve these baseline performance metrics in the production cluster, all heavy applications must strictly adhere to the following deployment rules:

1. **Guaranteed QoS:** The pod's CPU and Memory `requests` must exactly equal their `limits`.  
2. **Integer CPU Limits:** CPU requests/limits must be whole numbers (e.g., `40`, not `40.5`), otherwise the Kubelet will silently ignore the pinning request.  
3. **Strict Pod Anti-Affinity:** Applications must repel each other to ensure they land on dedicated nodes.

### **Standard Anti-Affinity Manifest Example**

```yaml
        affinity:
        podAntiAffinity:
            requiredDuringSchedulingIgnoredDuringExecution:
            - labelSelector:
                matchExpressions:
                - key: app
                operator: In
                values:
                - heavy-app-a
                - heavy-app-b
            topologyKey: "kubernetes.io/hostname"
```

## **8. OpenShift Infrastructure Manifests**

The following infrastructure files establish the `hpc` node tier to enable the performance benchmarks achieved above.

### **A. `hpc-mcp.yaml`**

Defines the MachineConfigPool for the dedicated High-Performance nodes.

```yaml
apiVersion: machineconfiguration.openshift.io/v1
kind: MachineConfigPool
metadata:
  name: hpc
  labels:
    custom-kubelet: hpc-tuning
spec:
  machineConfigSelector:
    matchExpressions:
      - {key: machineconfiguration.openshift.io/role, operator: In, values: [worker, hpc]}
  nodeSelector:
    matchLabels:
      node-role.kubernetes.io/hpc: ""
```

### **B. `hpc-kubeletconfig.yaml`**

Enables `static` CPU pinning and protects OpenShift infrastructure components by reserving dedicated cores away from the application space.

```yaml
apiVersion: machineconfiguration.openshift.io/v1
kind: KubeletConfig
metadata:
  name: hpc-tuning-app
spec:
  machineConfigPoolSelector:
    matchLabels:
      custom-kubelet: hpc-tuning
  kubeletConfig:
    cpuManagerPolicy: static
    topologyManagerPolicy: single-numa-node
    # Protect OpenShift infrastructure from application starvation
    systemReserved:
      cpu: "4"
      memory: "8Gi"
    kubeReserved:
      cpu: "4"
      memory: "8Gi"
    evictionHard:
      memory.available: "2Gi"
```

### **C. `hpc-profile.yaml` (For massive apps crossing NUMA boundaries)**

For ultra-heavy applications (e.g., 90 CPU limits) that must cross NUMA sockets, the Node Tuning Operator is required to isolate cores at the kernel boot level.

```yaml
apiVersion: performance.openshift.io/v2
kind: PerformanceProfile
metadata:
  name: hpc-massive-profile
spec:
  cpu:
    # 1. THE FENCE: Reserve CPUs for OpenShift/OS spread evenly across sockets
    reserved: "0-18,32-50"
    # 2. THE VAULT: Isolate exactly 90 CPUs strictly for the massive App.
    isolated: "19-31,51-127"
  numa:
    # Acknowledges the app must cross sockets, but aligns resources optimally
    topologyPolicy: "restricted"
  nodeSelector:
    node-role.kubernetes.io/hpc: ""
  realTimeKernel:
    enabled: false
```
