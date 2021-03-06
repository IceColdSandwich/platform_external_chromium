// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Copyright (c) 2011, Code Aurora Forum. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_HOST_RESOLVER_IMPL_H_
#define NET_BASE_HOST_RESOLVER_IMPL_H_
#pragma once

#include <vector>

#include "base/memory/scoped_ptr.h"
#include "base/threading/non_thread_safe.h"
#include "net/base/capturing_net_log.h"
#include "net/base/host_cache.h"
#include "net/base/host_resolver.h"
#include "net/base/host_resolver_proc.h"
#include "net/base/net_log.h"
#include "net/base/network_change_notifier.h"

namespace net {

// For each hostname that is requested, HostResolver creates a
// HostResolverImpl::Job. This job gets dispatched to a thread in the global
// WorkerPool, where it runs SystemHostResolverProc(). If requests for that same
// host are made while the job is already outstanding, then they are attached
// to the existing job rather than creating a new one. This avoids doing
// parallel resolves for the same host.
//
// The way these classes fit together is illustrated by:
//
//
//            +----------- HostResolverImpl -------------+
//            |                    |                     |
//           Job                  Job                   Job
//    (for host1, fam1)    (for host2, fam2)     (for hostx, famx)
//       /    |   |            /   |   |             /   |   |
//   Request ... Request  Request ... Request   Request ... Request
//  (port1)     (port2)  (port3)      (port4)  (port5)      (portX)
//
//
// When a HostResolverImpl::Job finishes its work in the threadpool, the
// callbacks of each waiting request are run on the origin thread.
//
// Thread safety: This class is not threadsafe, and must only be called
// from one thread!
//
// The HostResolverImpl enforces |max_jobs_| as the maximum number of concurrent
// threads.
//
// Requests are ordered in the queue based on their priority.

class HostResolverImpl : public HostResolver,
                         public base::NonThreadSafe,
                         public NetworkChangeNotifier::IPAddressObserver {
 public:
  // The index into |job_pools_| for the various job pools. Pools with a higher
  // index have lower priority.
  //
  // Note: This is currently unused, since there is a single pool
  //       for all requests.
  enum JobPoolIndex {
    POOL_NORMAL = 0,
    POOL_COUNT,
  };

  // Creates a HostResolver that first uses the local cache |cache|, and then
  // falls back to |resolver_proc|.
  //
  // If |cache| is NULL, then no caching is used. Otherwise we take
  // ownership of the |cache| pointer, and will free it during destructor.
  //
  // |resolver_proc| is used to perform the actual resolves; it must be
  // thread-safe since it is run from multiple worker threads. If
  // |resolver_proc| is NULL then the default host resolver procedure is
  // used (which is SystemHostResolverProc except if overridden).
  // |max_jobs| specifies the maximum number of threads that the host resolver
  // will use. Use SetPoolConstraints() to specify finer-grain settings.
  //
  // |net_log| must remain valid for the life of the HostResolverImpl.
  HostResolverImpl(HostResolverProc* resolver_proc,
                   HostCache* cache,
                   size_t max_jobs,
                   NetLog* net_log,
                   MessageLoop* net_notification_messageloop=NULL
                   );

  // If any completion callbacks are pending when the resolver is destroyed,
  // the host resolutions are cancelled, and the completion callbacks will not
  // be called.
  virtual ~HostResolverImpl();

  // Continuously observe whether IPv6 is supported, and set the allowable
  // address family to IPv4 iff IPv6 is not supported.
  void ProbeIPv6Support();

  virtual void ForgetHostPortPair(const net::HostPortPair& host_pair);

  // Returns the cache this resolver uses, or NULL if caching is disabled.
  HostCache* cache() { return cache_.get(); }

  // Applies a set of constraints for requests that belong to the specified
  // pool. NOTE: Don't call this after requests have been already been started.
  //
  //  |pool_index| -- Specifies which pool these constraints should be applied
  //                  to.
  //  |max_outstanding_jobs| -- How many concurrent jobs are allowed for this
  //                            pool.
  //  |max_pending_requests| -- How many requests can be enqueued for this pool
  //                            before we start dropping requests. Dropped
  //                            requests fail with
  //                            ERR_HOST_RESOLVER_QUEUE_TOO_LARGE.
  void SetPoolConstraints(JobPoolIndex pool_index,
                          size_t max_outstanding_jobs,
                          size_t max_pending_requests);

  // HostResolver methods:
  virtual int Resolve(const RequestInfo& info,
                      AddressList* addresses,
                      CompletionCallback* callback,
                      RequestHandle* out_req,
                      const BoundNetLog& source_net_log);
  virtual void CancelRequest(RequestHandle req);
  virtual void AddObserver(HostResolver::Observer* observer);
  virtual void RemoveObserver(HostResolver::Observer* observer);

  // Set address family, and disable IPv6 probe support.
  virtual void SetDefaultAddressFamily(AddressFamily address_family);
  virtual AddressFamily GetDefaultAddressFamily() const;

  virtual HostResolverImpl* GetAsHostResolverImpl();

  // TODO(eroman): hack for http://crbug.com/15513
  virtual void Shutdown();

  virtual void SetResolverExt(HostnameResolverExt* preresolver);
 private:
  class Job;
  class JobPool;
  class IPv6ProbeJob;
  class Request;
  typedef std::vector<Request*> RequestsList;
  typedef HostCache::Key Key;
  typedef std::map<Key, scoped_refptr<Job> > JobMap;
  typedef std::vector<HostResolver::Observer*> ObserversList;

  // Returns the HostResolverProc to use for this instance.
  HostResolverProc* effective_resolver_proc() const {
    return resolver_proc_ ?
        resolver_proc_.get() : HostResolverProc::GetDefault();
  }

  // Adds a job to outstanding jobs list.
  void AddOutstandingJob(Job* job);

  // Returns the outstanding job for |key|, or NULL if there is none.
  Job* FindOutstandingJob(const Key& key);

  // Removes |job| from the outstanding jobs list.
  void RemoveOutstandingJob(Job* job);

  // Callback for when |job| has completed with |net_error| and |addrlist|.
  void OnJobComplete(Job* job, int net_error, int os_error,
                     const AddressList& addrlist);

  // Aborts |job|.  Same as OnJobComplete() except does not remove |job|
  // from |jobs_| and does not cache the result (ERR_ABORTED).
  void AbortJob(Job* job);

  // Used by both OnJobComplete() and AbortJob();
  void OnJobCompleteInternal(Job* job, int net_error, int os_error,
                             const AddressList& addrlist);

  // Called when a request has just been started.
  void OnStartRequest(const BoundNetLog& source_net_log,
                      const BoundNetLog& request_net_log,
                      int request_id,
                      const RequestInfo& info);

  // Called when a request has just completed (before its callback is run).
  void OnFinishRequest(const BoundNetLog& source_net_log,
                       const BoundNetLog& request_net_log,
                       int request_id,
                       const RequestInfo& info,
                       int net_error,
                       int os_error);

  // Called when a request has been cancelled.
  void OnCancelRequest(const BoundNetLog& source_net_log,
                       const BoundNetLog& request_net_log,
                       int request_id,
                       const RequestInfo& info);

  // Notify IPv6ProbeJob not to call back, and discard reference to the job.
  void DiscardIPv6ProbeJob();

  // Callback from IPv6 probe activity.
  void IPv6ProbeSetDefaultAddressFamily(AddressFamily address_family);

  // Returns true if the constraints for |pool| are met, and a new job can be
  // created for this pool.
  bool CanCreateJobForPool(const JobPool& pool) const;

  // Returns the index of the pool that request |req| maps to.
  static JobPoolIndex GetJobPoolIndexForRequest(const Request* req);

  JobPool* GetPoolForRequest(const Request* req) {
    return job_pools_[GetJobPoolIndexForRequest(req)];
  }

  // Starts up to 1 job given the current pool constraints. This job
  // may have multiple requests attached to it.
  void ProcessQueuedRequests();

  // Returns the (hostname, address_family) key to use for |info|, choosing an
  // "effective" address family by inheriting the resolver's default address
  // family when the request leaves it unspecified.
  Key GetEffectiveKeyForRequest(const RequestInfo& info) const;

  // Attaches |req| to a new job, and starts it. Returns that job.
  Job* CreateAndStartJob(Request* req);

  // Adds a pending request |req| to |pool|.
  int EnqueueRequest(JobPool* pool, Request* req);

  // Cancels all jobs.
  void CancelAllJobs();

  // Aborts all in progress jobs (but might start new ones).
  void AbortAllInProgressJobs();

  // NetworkChangeNotifier::IPAddressObserver methods:
  virtual void OnIPAddressChanged();

  // Cache of host resolution results.
  scoped_ptr<HostCache> cache_;

  // Map from hostname to outstanding job.
  JobMap jobs_;

  // Maximum number of concurrent jobs allowed, across all pools.
  size_t max_jobs_;

  // The information to track pending requests for a JobPool, as well as
  // how many outstanding jobs the pool already has, and its constraints.
  JobPool* job_pools_[POOL_COUNT];

  // The job that OnJobComplete() is currently processing (needed in case
  // HostResolver gets deleted from within the callback).
  scoped_refptr<Job> cur_completing_job_;

  // The observers to notify when a request starts/ends.
  ObserversList observers_;

  // Monotonically increasing ID number to assign to the next request.
  // Observers are the only consumers of this ID number.
  int next_request_id_;

  // Monotonically increasing ID number to assign to the next job.
  // The only consumer of this ID is the requests tracing code.
  int next_job_id_;

  // The procedure to use for resolving host names. This will be NULL, except
  // in the case of unit-tests which inject custom host resolving behaviors.
  scoped_refptr<HostResolverProc> resolver_proc_;

  // Address family to use when the request doesn't specify one.
  AddressFamily default_address_family_;

  // TODO(eroman): hack for http://crbug.com/15513
  bool shutdown_;

  // Indicate if probing is done after each network change event to set address
  // family.
  // When false, explicit setting of address family is used.
  bool ipv6_probe_monitoring_;

  // The last un-cancelled IPv6ProbeJob (if any).
  scoped_refptr<IPv6ProbeJob> ipv6_probe_job_;

  // Any resolver flags that should be added to a request by default.
  HostResolverFlags additional_resolver_flags_;

  NetLog* net_log_;

  MessageLoop* net_notification_messageloop_;
  HostnameResolverExt* resolverext_;

  DISALLOW_COPY_AND_ASSIGN(HostResolverImpl);
};

}  // namespace net

#endif  // NET_BASE_HOST_RESOLVER_IMPL_H_
