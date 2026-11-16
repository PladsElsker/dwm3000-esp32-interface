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


const CONFIG_PRESETS_FILE: &str = "configuration-presets.json";
const MAX_CONFIG_PRESETS_BYTES: usize = 2 * 1024 * 1024;

/// Load the application-level configuration set. Unlike WebView localStorage,
/// app_data_dir remains stable when the frontend is rebuilt or the app is updated.
#[tauri::command]
fn load_config_presets(app: AppHandle) -> Result<Option<String>, String> {
    let path = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("Could not resolve the D3KM data directory: {e}"))?
        .join(CONFIG_PRESETS_FILE);

    match std::fs::read_to_string(&path) {
        Ok(contents) => {
            if contents.len() > MAX_CONFIG_PRESETS_BYTES {
                return Err("Saved configuration set exceeds the 2 MB limit.".into());
            }
            let value: serde_json::Value = serde_json::from_str(&contents)
                .map_err(|e| format!("Saved configuration set is not valid JSON: {e}"))?;
            if !value.is_array() {
                return Err("Saved configuration set must contain a JSON array.".into());
            }
            Ok(Some(contents))
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(format!("Could not read saved configurations: {e}")),
    }
}

/// Persist the complete local configuration set outside the frontend build.
#[tauri::command]
fn save_config_presets(presets_json: String, app: AppHandle) -> Result<(), String> {
    if presets_json.len() > MAX_CONFIG_PRESETS_BYTES {
        return Err("Configuration set exceeds the 2 MB storage limit.".into());
    }

    let value: serde_json::Value = serde_json::from_str(&presets_json)
        .map_err(|e| format!("Configuration set is not valid JSON: {e}"))?;
    if !value.is_array() {
        return Err("Configuration set must be a JSON array.".into());
    }

    let data_dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("Could not resolve the D3KM data directory: {e}"))?;
    std::fs::create_dir_all(&data_dir)
        .map_err(|e| format!("Could not create the D3KM data directory: {e}"))?;
    let path = data_dir.join(CONFIG_PRESETS_FILE);
    std::fs::write(&path, presets_json)
        .map_err(|e| format!("Could not save configurations to {}: {e}", path.display()))
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
            send_command,
            load_config_presets,
            save_config_presets
        ])
        .run(tauri::generate_context!())
        .expect("error while running D3KM");
}
