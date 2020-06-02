// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Copied from src/ui/bin/brightness_manager
// TODO(fxb/36843) consolidate usages

use std::path::Path;
use std::{fs, io};

use anyhow::{format_err, Context as _, Error};
use fidl_fuchsia_input_report::{
    InputDeviceMarker, InputDeviceProxy, InputReport, SensorAxis, SensorDescriptor,
    SensorInputDescriptor, SensorType,
};
use fuchsia_async as fasync;
use fuchsia_syslog::{fx_log_err, fx_log_info};
use fuchsia_zircon as zx;

#[derive(Debug)]
pub struct AmbientLightInputRpt {
    pub rpt_id: u64,
    pub illuminance: i64,
    pub red: i64,
    pub green: i64,
    pub blue: i64,
}

#[derive(Debug, Clone)]
pub struct Sensor {
    proxy: InputDeviceProxy,
    sensor_axes: Vec<SensorAxis>,
}

impl Sensor {
    pub fn new(proxy: InputDeviceProxy, sensor_axes: Vec<SensorAxis>) -> Self {
        Self { proxy, sensor_axes }
    }
}

/// Opens the sensor's device file.
/// Tries all the input devices until the one with the correct signature is found.
pub async fn open_sensor() -> Result<Sensor, Error> {
    const INPUT_DEVICES_DIRECTORY: &str = "/dev/class/input-report";
    let path = Path::new(INPUT_DEVICES_DIRECTORY);
    let entries = fs::read_dir(path)?;
    for entry in entries {
        let entry = entry?;
        let device = open_input_device(entry.path().to_str().expect("Bad path"))?;
        let res = device.get_descriptor().await;
        if let Ok(device_descriptor) = res {
            if let Some(SensorDescriptor {
                input: Some(SensorInputDescriptor { values: Some(values) }),
            }) = device_descriptor.sensor
            {
                let mut illuminance = false;
                let mut red = false;
                let mut green = false;
                let mut blue = false;
                for sensor_axis in &values {
                    match sensor_axis.type_ {
                        SensorType::LightIlluminance => illuminance = true,
                        SensorType::LightRed => red = true,
                        SensorType::LightGreen => green = true,
                        SensorType::LightBlue => blue = true,
                        _ => (),
                    }
                }

                if illuminance && red && green && blue {
                    return Ok(Sensor::new(device, values));
                }
            }
        }
    }

    Err(io::Error::new(io::ErrorKind::NotFound, "no sensor found").into())
}

fn open_input_device(path: &str) -> Result<InputDeviceProxy, Error> {
    fx_log_info!("Opening sensor at {:?}", path);
    let (proxy, server) = fidl::endpoints::create_proxy::<InputDeviceMarker>()
        .context("Failed to create sensor proxy")?;
    fdio::service_connect(path, server.into_channel())
        .context("Failed to connect built-in service")?;
    Ok(proxy)
}

pub async fn get_reports(sensor: &Sensor) -> Result<Option<InputReport>, Error> {
    let reports = sensor.proxy.get_reports().await?;
    fx_log_info!("Got reports: {:?}", reports);
    Ok(reports.into_iter().max_by_key(|r| r.event_time.unwrap_or(0)))
}

/// Reads the sensor's HID record and decodes it.
pub async fn read_sensor(sensor: &Sensor) -> Result<AmbientLightInputRpt, Error> {
    let report = match get_reports(sensor).await? {
        Some(report) => report,
        None => {
            let (status, event) = sensor
                .proxy
                .get_reports_event()
                .await
                .map(|(status, event)| (fuchsia_zircon::Status::from_raw(status), event))?;
            if status != fuchsia_zircon::Status::OK {
                fx_log_err!("Failed to get reports event");
                return Err(format_err!("Failed to wait on reports event {:?}", status));
            }

            fx_log_info!("Waiting on readable event...");
            fasync::OnSignals::new(&event, zx::Signals::USER_0).await?;
            fx_log_info!("Got event! Getting reports...");
            get_reports(sensor)
                .await?
                .ok_or_else(|| format_err!("Failed to get a device report"))?
        }
    };
    fx_log_info!("Got report {:?}", report);

    let rpt_id = report.trace_id.unwrap();
    let report = report.sensor.unwrap();
    let values = report.values.unwrap();
    let mut illuminance = None;
    let mut red = None;
    let mut green = None;
    let mut blue = None;
    fx_log_info!("PAUL values: {:?}", values);
    for (sensor_axis, value) in sensor.sensor_axes.iter().zip(values.into_iter()) {
        match sensor_axis.type_ {
            SensorType::LightIlluminance => {
                illuminance = Some(value);
            }
            SensorType::LightRed => {
                red = Some(value);
            }
            SensorType::LightGreen => {
                green = Some(value);
            }
            SensorType::LightBlue => blue = Some(value),
            _ => {}
        }
    }

    if let (Some(illuminance), Some(red), Some(green), Some(blue)) = (illuminance, red, green, blue)
    {
        return Ok(AmbientLightInputRpt { rpt_id, illuminance, red, green, blue });
    } else {
        Err(format_err!("Missing light data from sensor report"))
    }
}

#[cfg(test)]
pub mod testing {
    use fidl_fuchsia_input_report::{
        Axis, InputReport, Range, SensorAxis, SensorInputReport, SensorType, Unit,
    };

    pub const TEST_LUX_VAL: i64 = 605;
    pub const TEST_RED_VAL: i64 = 345;
    pub const TEST_BLUE_VAL: i64 = 133;
    pub const TEST_GREEN_VAL: i64 = 164;

    pub fn get_mock_sensor_response() -> (Vec<SensorAxis>, impl Fn() -> Vec<InputReport>) {
        let axis = Axis { range: Range { min: 0, max: 1000 }, unit: Unit::Lux };
        (
            vec![
                SensorAxis { axis: axis.clone(), type_: SensorType::LightRed },
                SensorAxis { axis: axis.clone(), type_: SensorType::LightIlluminance },
                SensorAxis { axis: axis.clone(), type_: SensorType::LightBlue },
                SensorAxis { axis: axis.clone(), type_: SensorType::LightGreen },
            ],
            || {
                vec![InputReport {
                    event_time: Some(65),
                    mouse: None,
                    trace_id: Some(45),
                    sensor: Some(SensorInputReport {
                        values: Some(vec![
                            TEST_RED_VAL,
                            TEST_LUX_VAL,
                            TEST_BLUE_VAL,
                            TEST_GREEN_VAL,
                        ]),
                    }),
                    touch: None,
                    keyboard: None,
                    consumer_control: None,
                }]
            },
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl_fuchsia_input_report::InputDeviceRequest;
    use fuchsia_async as fasync;
    use futures::prelude::*;
    use testing;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_read_sensor() {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<InputDeviceMarker>().unwrap();
        let (axes, data_fn) = testing::get_mock_sensor_response();
        fasync::spawn(async move {
            while let Some(request) = stream.try_next().await.unwrap() {
                if let InputDeviceRequest::GetReports { responder } = request {
                    let data = data_fn();
                    responder.send(&mut data.into_iter()).unwrap();
                }
            }
        });

        let sensor = Sensor::new(proxy, axes);

        let result = read_sensor(&sensor).await;
        match result {
            Ok(input_rpt) => {
                assert_eq!(input_rpt.illuminance, testing::TEST_LUX_VAL);
                assert_eq!(input_rpt.red, testing::TEST_RED_VAL);
                assert_eq!(input_rpt.green, testing::TEST_GREEN_VAL);
                assert_eq!(input_rpt.blue, testing::TEST_BLUE_VAL);
            }
            Err(e) => {
                panic!("Sensor read failed: {:?}", e);
            }
        }
    }
}
