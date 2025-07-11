// Copyright 2025 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cobalt/browser/metrics/cobalt_metrics_service_client.h"

#include <memory>

#include "base/logging.h"
#include "base/notreached.h"
#include "base/posix/file_descriptor_shuffle.h"
#include "base/time/time.h"
#include "base/version.h"
#include "cobalt/browser/metrics/cobalt_metrics_log_uploader.h"
#include "components/metrics/metrics_service.h"
#include "components/metrics/metrics_state_manager.h"
#include "components/prefs/pref_service.h"
#include "components/variations/synthetic_trial_registry.h"
#include "url/gurl.h"

namespace cobalt {

CobaltMetricsServiceClient::CobaltMetricsServiceClient(
    metrics::MetricsStateManager* state_manager,
    std::unique_ptr<variations::SyntheticTrialRegistry>
        synthetic_trial_registry,
    PrefService* local_state)
    : metrics_state_manager_(state_manager),
      synthetic_trial_registry_(std::move(synthetic_trial_registry)),
      local_state_(local_state) {
  DETACH_FROM_THREAD(thread_checker_);
}

void CobaltMetricsServiceClient::Initialize() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  metrics_service_ = CreateMetricsServiceInternal(metrics_state_manager_.get(),
                                                  this, local_state_.get());
  log_uploader_ = CreateLogUploaderInternal();
  log_uploader_weak_ptr_ = log_uploader_->GetWeakPtr();
  StartIdleRefreshTimer();
}

void CobaltMetricsServiceClient::StartIdleRefreshTimer() {
  if (idle_refresh_timer_.IsRunning()) {
    idle_refresh_timer_.Stop();
  }

  // At a rate of half the upload interval, we force UMA to consider the app
  // as non-idle. This guarantees metrics are uploaded regularly. This is done
  // for two reasons:
  //
  //   1) The nature of the YouTube application is such that the user can be
  //      "idle" for long periods (e.g., watching a movie). We still want to
  //      send metrics in these cases.
  //
  //   2) The typical way Chromium handles non-idle is page loads and user
  //      actions. User actions are currently sparse and/or not working due to
  //      the nature of Kabuki's implementation (see b/417477183).
  auto timer_interval = GetStandardUploadInterval() / 2;
  timer_interval = timer_interval > kMinIdleRefreshInterval
                       ? timer_interval
                       : kMinIdleRefreshInterval;
  idle_refresh_timer_.Start(
      FROM_HERE, timer_interval, this,
      &CobaltMetricsServiceClient::OnApplicationNotIdleInternal);
  DLOG(INFO) << "Starting refresh timer for: "
             << idle_refresh_timer_.GetCurrentDelay().InSeconds() << " seconds";
}

std::unique_ptr<metrics::MetricsService>
CobaltMetricsServiceClient::CreateMetricsServiceInternal(
    metrics::MetricsStateManager* state_manager,
    metrics::MetricsServiceClient* client,
    PrefService* local_state) {
  return std::make_unique<metrics::MetricsService>(state_manager, client,
                                                   local_state);
}

// static
std::unique_ptr<CobaltMetricsServiceClient> CobaltMetricsServiceClient::Create(
    metrics::MetricsStateManager* state_manager,
    std::unique_ptr<variations::SyntheticTrialRegistry>
        synthetic_trial_registry,
    PrefService* local_state) {
  // Perform two-phase initialization so that `client->metrics_service_` only
  // receives pointers to fully constructed objects.
  std::unique_ptr<CobaltMetricsServiceClient> client(
      new CobaltMetricsServiceClient(
          state_manager, std::move(synthetic_trial_registry), local_state));
  client->Initialize();

  return client;
}

variations::SyntheticTrialRegistry*
CobaltMetricsServiceClient::GetSyntheticTrialRegistry() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  return synthetic_trial_registry_.get();
}

metrics::MetricsService* CobaltMetricsServiceClient::GetMetricsService() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  return metrics_service_.get();
}

void CobaltMetricsServiceClient::SetMetricsClientId(
    const std::string& client_id) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // ClientId is unnecessary within Cobalt. We expect the web client responsible
  // for uploading these to have its own concept of device/client identifiers.
}

// TODO(b/286884542): Audit all stub implementations in this class and reaffirm
// they're not needed and/or add a reasonable implementation.
int32_t CobaltMetricsServiceClient::GetProduct() {
  // Note, Product is a Chrome concept and similar dimensions will get logged
  // elsewhere downstream. This value doesn't matter.
  return 0;
}

std::string CobaltMetricsServiceClient::GetApplicationLocale() {
  // The locale will be populated by the web client, so return value is
  // inconsequential.
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  return "en-US";
}

const network_time::NetworkTimeTracker*
CobaltMetricsServiceClient::GetNetworkTimeTracker() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // TODO(b/372559349): Figure out whether we need to return a real object.
  // The NetworkTimeTracker used to provide higher-quality wall clock times than
  // |clock_| (when available). Can be overridden for tests.
  NOTIMPLEMENTED();
  return nullptr;
}

bool CobaltMetricsServiceClient::GetBrand(std::string* brand_code) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // "false" means no brand code available. We set the brand when uploading
  // via GEL.
  return false;
}

metrics::SystemProfileProto::Channel CobaltMetricsServiceClient::GetChannel() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // Return value here is unused in downstream logging.
  return metrics::SystemProfileProto::CHANNEL_UNKNOWN;
}

bool CobaltMetricsServiceClient::IsExtendedStableChannel() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  return false;  // Not supported on Cobalt.
}

std::string CobaltMetricsServiceClient::GetVersionString() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // E.g. 134.0.6998.19.
  return base::Version().GetString();
}

void CobaltMetricsServiceClient::CollectFinalMetricsForLog(
    base::OnceClosure done_callback) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // Any hooks that should be called before each new log is uploaded, goes here.
  // Chrome uses this to update memory histograms. Regardless, you must call
  // done_callback when done else the uploader will never get invoked.
  std::move(done_callback).Run();

  OnApplicationNotIdleInternal();
}
void CobaltMetricsServiceClient::OnApplicationNotIdleInternal() {
  // MetricsService will shut itself down if the app doesn't periodically tell
  // it it's not idle. In Cobalt's case, we don't want this behavior. Watch
  // sessions for LR can happen for extended periods of time with no action by
  // the user. So, we always just set the app as "non-idle" immediately after
  // each metric log is finalized.
  GetMetricsService()->OnApplicationNotIdle();
}

GURL CobaltMetricsServiceClient::GetMetricsServerUrl() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // Chrome keeps the actual URL in an internal file, likely to avoid abuse.
  // This below is made up, and in any case likely not to be used (it ends up in
  // CreateUploader()'s `server_url`. Return empty and use instead logic in
  // CobaltMetricsLogUploader.
  return GURL("https://youtube.com/tv/uma");
}

std::unique_ptr<metrics::MetricsLogUploader>
CobaltMetricsServiceClient::CreateUploader(
    const GURL& server_url,
    const GURL& insecure_server_url,
    base::StringPiece mime_type,
    metrics::MetricsLogUploader::MetricServiceType service_type,
    const metrics::MetricsLogUploader::UploadCallback& on_upload_complete) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  // Uploader should already be initialized early in construction.
  CHECK(log_uploader_);
  log_uploader_->setOnUploadComplete(on_upload_complete);
  return std::move(log_uploader_);
}

std::unique_ptr<CobaltMetricsLogUploader>
CobaltMetricsServiceClient::CreateLogUploaderInternal() {
  return std::make_unique<CobaltMetricsLogUploader>();
}

base::TimeDelta CobaltMetricsServiceClient::GetStandardUploadInterval() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(IsInitialized());
  return upload_interval_;
}

void CobaltMetricsServiceClient::SetUploadInterval(base::TimeDelta interval) {
  upload_interval_ = interval;
  // If upload interval changes, update idle refresh timer accordingly.
  StartIdleRefreshTimer();
}

void CobaltMetricsServiceClient::SetMetricsListener(
    ::mojo::PendingRemote<::h5vcc_metrics::mojom::MetricsListener> listener) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  log_uploader_weak_ptr_->SetMetricsListener(std::move(listener));
}
}  // namespace cobalt
