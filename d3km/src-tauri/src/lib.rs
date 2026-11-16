mod protocol;

use serialport::{SerialPort, SerialPortInfo};
use std::sync::Mutex;
use tauri::{AppHandle, Manager};

const OP_UNSUBSCRIBE: u8 = 0x0A;
// Event bits 0–14 cover the UWB and BLE events defined by firmware Protocol v2.0.
const ALL_EVENT_MASK_LE: [u8; 2] = [0xFF, 0x7F];

struct UsbState {
    port: Option<Box<dyn SerialPort>>,
    seq: u16,
    rx_encoded: Vec<u8>,
}

struct Device(Mutex<UsbState>);

#[derive(serde::Serialize)]
struct PortInfo {
    name: String,
    description: String,
}

fn next_sequence(usb: &mut UsbState) -> u16 {
    let seq = usb.seq;
    usb.seq = usb.seq.wrapping_add(1);
    if usb.seq == 0 {
        usb.seq = 1;
    }
    seq
}

/// Best-effort graceful unsubscribe. The command and its response are
/// exchanged before the OS serial handle is dropped. If the cable was already
/// removed, the command may fail; firmware-side CDC detection handles that case.
fn unsubscribe_before_close(usb: &mut UsbState, app: &AppHandle) -> bool {
    if usb.port.is_none() {
        return false;
    }

    // Discard any partial frame left by an interrupted previous read so the
    // unsubscribe response is parsed from a clean COBS frame boundary.
    usb.rx_encoded.clear();
    let seq = next_sequence(usb);

    let UsbState {
        port, rx_encoded, ..
    } = usb;

    let port = match port.as_mut() {
        Some(port) => port,
        None => return false,
    };

    match protocol::command(
        port,
        app,
        seq,
        OP_UNSUBSCRIBE,
        &ALL_EVENT_MASK_LE,
        rx_encoded,
    ) {
        Ok(reply) => reply.payload.as_slice() == [0, 0],
        Err(_) => false,
    }
}

fn is_serial_disconnect_error(result: &Result<protocol::Reply, String>) -> bool {
    match result {
        Err(e) => {
            e.starts_with("USB write failed:")
                || e.starts_with("USB flush failed:")
                || e.starts_with("USB read failed:")
        }
        Ok(_) => false,
    }
}

#[tauri::command]
async fn list_ports() -> Vec<PortInfo> {
    tauri::async_runtime::spawn_blocking(|| {
        serialport::available_ports()
            .unwrap_or_default()
            .into_iter()
            .map(|p: SerialPortInfo| {
                let description = match p.port_type {
                    serialport::SerialPortType::UsbPort(u) => format!(
                        "{} {} · {}",
                        u.manufacturer.unwrap_or_default(),
                        u.product.unwrap_or_default(),
                        u.serial_number.unwrap_or_default()
                    ),
                    serialport::SerialPortType::BluetoothPort => "Bluetooth serial".into(),
                    serialport::SerialPortType::PciPort => "PCI serial".into(),
                    serialport::SerialPortType::Unknown => "Serial device".into(),
                };

                PortInfo {
                    name: p.port_name,
                    description,
                }
            })
            .collect()
    })
    .await
    .unwrap_or_default()
}

#[tauri::command]
async fn connect(port_name: String, app: AppHandle) -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let state = app.state::<Device>();
        let mut usb = state.0.lock().map_err(|e| e.to_string())?;

        // If switching ports, unsubscribe from the old device before dropping
        // its serial handle. Failure is non-fatal: physical disconnect detection
        // in the firmware clears that session's subscription state.
        let _ = unsubscribe_before_close(&mut usb, &app);
        usb.port = None;
        usb.rx_encoded.clear();

        let port = serialport::new(&port_name, 115200)
            .timeout(std::time::Duration::from_millis(25))
            .open()
            .map_err(|e| format!("Could not open {port_name}: {e}"))?;

        usb.port = Some(port);
        usb.seq = 1;
        usb.rx_encoded.clear();

        Ok(port_name)
    })
    .await
    .map_err(|e| format!("USB connect worker failed: {e}"))?
}

#[tauri::command]
async fn disconnect(app: AppHandle) -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(move || {
        let state = app.state::<Device>();
        let mut usb = state.0.lock().map_err(|e| e.to_string())?;

        // Send UNSUBSCRIBE(0x7FFF), wait for its acknowledgement, then close.
        // If the cable is already gone, still complete local disconnect; the
        // firmware independently detects the lost CDC session.
        let _unsubscribe_acknowledged = unsubscribe_before_close(&mut usb, &app);
        usb.port = None;
        usb.rx_encoded.clear();
        Ok(())
    })
    .await
    .map_err(|e| format!("USB disconnect worker failed: {e}"))?
}

#[tauri::command]
async fn send_command(
    opcode: u8,
    payload: Vec<u8>,
    app: AppHandle,
) -> Result<protocol::Reply, String> {
    // Serial reads are blocking. Run the complete request/response operation
    // on Tauri's blocking pool so it cannot block the WebView/UI thread.
    tauri::async_runtime::spawn_blocking(move || {
        let state = app.state::<Device>();
        let mut usb = state.0.lock().map_err(|e| e.to_string())?;
        let seq = next_sequence(&mut usb);

        let result = {
            let UsbState {
                port, rx_encoded, ..
            } = &mut *usb;

            let port = port
                .as_mut()
                .ok_or_else(|| "No USB device connected".to_string())?;

            protocol::command(port, &app, seq, opcode, &payload, rx_encoded)
        };

        // A hard OS-level I/O error means the USB serial handle is no longer
        // usable. Drop it so the frontend stops treating this session as live.
        // A protocol response timeout alone does not force a disconnect.
        if is_serial_disconnect_error(&result) {
            usb.port = None;
            usb.rx_encoded.clear();
        }

        result
    })
    .await
    .map_err(|e| format!("USB command worker failed: {e}"))?
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .manage(Device(Mutex::new(UsbState {
            port: None,
            seq: 1,
            rx_encoded: Vec::new(),
        })))
        .invoke_handler(tauri::generate_handler![
            list_ports,
            connect,
            disconnect,
            send_command
        ])
        .run(tauri::generate_context!())
        .expect("error while running D3KM");
}
