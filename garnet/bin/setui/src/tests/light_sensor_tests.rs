// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
use {
    crate::display::{light_sensor_testing, LIGHT_SENSOR_SERVICE_NAME},
    crate::registry::device_storage::testing::*,
    crate::switchboard::base::SettingType,
    crate::EnvironmentBuilder,
    anyhow::format_err,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_input_report::{
        DeviceDescriptor, InputDeviceRequest, SensorDescriptor, SensorInputDescriptor,
    },
    fidl_fuchsia_settings::*,
    fuchsia_async as fasync, fuchsia_zircon as zx,
    futures::future::BoxFuture,
    futures::prelude::*,
};

const ENV_NAME: &str = "settings_service_light_sensor_test_environment";

#[fuchsia_async::run_singlethreaded(test)]
async fn test_light_sensor() {
    let service_gen = |service_name: &str,
                       channel: zx::Channel|
     -> BoxFuture<'static, Result<(), anyhow::Error>> {
        if service_name != LIGHT_SENSOR_SERVICE_NAME {
            let service = String::from(service_name);
            return Box::pin(async move { Err(format_err!("{:?} unsupported!", service)) });
        }

        let stream_result =
            ServerEnd::<fidl_fuchsia_input_report::InputDeviceMarker>::new(channel).into_stream();

        if stream_result.is_err() {
            return Box::pin(async { Err(format_err!("could not connect to service")) });
        }

        let mut stream = stream_result.unwrap();

        let (sensor_axes, data_fn) = light_sensor_testing::get_mock_sensor_response();
        fasync::spawn(async move {
            while let Some(request) = stream.try_next().await.unwrap() {
                match request {
                    InputDeviceRequest::GetReports { responder } => {
                        let data = data_fn();
                        responder.send(&mut data.into_iter()).unwrap();
                    }
                    InputDeviceRequest::GetDescriptor { responder } => {
                        responder
                            .send(DeviceDescriptor {
                                device_info: None,
                                mouse: None,
                                sensor: Some(SensorDescriptor {
                                    input: Some(SensorInputDescriptor {
                                        values: Some(sensor_axes.clone()),
                                    }),
                                }),
                                touch: None,
                                keyboard: None,
                                consumer_control: None,
                            })
                            .unwrap();
                    }
                    _ => {}
                }
            }
        });

        Box::pin(async { Ok(()) })
    };

    let env = EnvironmentBuilder::new(InMemoryStorageFactory::create())
        .service(Box::new(service_gen))
        .settings(&[SettingType::LightSensor])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    let display_service = env.connect_to_service::<DisplayMarker>().unwrap();
    let data = display_service
        .watch_light_sensor(0.0)
        .await
        .expect("watch completed")
        .expect("watch successful");

    // TODO check i64 -> f32?
    assert_eq!(data.illuminance_lux, Some(light_sensor_testing::TEST_LUX_VAL as f32));
    assert_eq!(
        data.color,
        Some(fidl_fuchsia_ui_types::ColorRgb {
            red: light_sensor_testing::TEST_RED_VAL as f32,
            green: light_sensor_testing::TEST_GREEN_VAL as f32,
            blue: light_sensor_testing::TEST_BLUE_VAL as f32,
        })
    );
}
