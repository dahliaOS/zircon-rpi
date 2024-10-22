// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::fidl_process;
use fidl_fuchsia_settings::{
    Error, PrivacyMarker, PrivacyRequest, PrivacySettings, PrivacyWatch2Responder,
    PrivacyWatchResponder,
};
use fuchsia_async as fasync;
use futures::future::LocalBoxFuture;
use futures::FutureExt;

use crate::fidl_hanging_get_responder;
use crate::fidl_hanging_get_result_responder;
use crate::fidl_processor::RequestContext;
use crate::request_respond;
use crate::switchboard::base::{SettingRequest, SettingResponse, SettingType};
use crate::switchboard::hanging_get_handler::Sender;

fidl_hanging_get_responder!(PrivacySettings, PrivacyWatch2Responder, PrivacyMarker::DEBUG_NAME);

// TODO(fxb/52593): Remove when clients are ported to watch2.
fidl_hanging_get_result_responder!(
    PrivacySettings,
    PrivacyWatchResponder,
    PrivacyMarker::DEBUG_NAME
);

impl From<SettingResponse> for PrivacySettings {
    fn from(response: SettingResponse) -> Self {
        if let SettingResponse::Privacy(info) = response {
            return PrivacySettings { user_data_sharing_consent: info.user_data_sharing_consent };
        }

        panic!("incorrect value sent to privacy");
    }
}

impl From<PrivacySettings> for SettingRequest {
    fn from(settings: PrivacySettings) -> Self {
        SettingRequest::SetUserDataSharingConsent(settings.user_data_sharing_consent)
    }
}

fidl_process!(
    Privacy,
    SettingType::Privacy,
    process_request,
    PrivacyWatch2Responder,
    process_request_2
);

// TODO(fxb/52593): Replace with logic from process_request_2
// and remove process_request_2 when clients ported to Watch2 and back.
async fn process_request(
    context: RequestContext<PrivacySettings, PrivacyWatchResponder>,
    req: PrivacyRequest,
) -> Result<Option<PrivacyRequest>, anyhow::Error> {
    #[allow(unreachable_patterns)]
    match req {
        PrivacyRequest::Watch { responder } => {
            context.watch(responder, false).await;
        }
        _ => {
            return Ok(Some(req));
        }
    }

    return Ok(None);
}

async fn process_request_2(
    context: RequestContext<PrivacySettings, PrivacyWatch2Responder>,
    req: PrivacyRequest,
) -> Result<Option<PrivacyRequest>, anyhow::Error> {
    #[allow(unreachable_patterns)]
    match req {
        PrivacyRequest::Set { settings, responder } => {
            fasync::spawn(async move {
                request_respond!(
                    context,
                    responder,
                    SettingType::Privacy,
                    settings.into(),
                    Ok(()),
                    Err(Error::Failed),
                    PrivacyMarker::DEBUG_NAME
                );
            });
        }
        PrivacyRequest::Watch2 { responder } => {
            context.watch(responder, true).await;
        }
        _ => {
            return Ok(Some(req));
        }
    }

    return Ok(None);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_request_from_settings_empty() {
        let request = SettingRequest::from(PrivacySettings::empty());

        assert_eq!(request, SettingRequest::SetUserDataSharingConsent(None));
    }

    #[test]
    fn test_request_from_settings() {
        const USER_DATA_SHARING_CONSENT: bool = true;

        let mut privacy_settings = PrivacySettings::empty();
        privacy_settings.user_data_sharing_consent = Some(USER_DATA_SHARING_CONSENT);

        let request = SettingRequest::from(privacy_settings);

        assert_eq!(
            request,
            SettingRequest::SetUserDataSharingConsent(Some(USER_DATA_SHARING_CONSENT))
        );
    }
}
