use serialport::SerialPort;
use std::io::{Read, Write};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter};

pub const ERRORS: [&str; 11] = [
    "OK",
    "BAD_ARGUMENT",
    "UNKNOWN_COMMAND",
    "BAD_STATE",
    "BUSY",
    "NOT_SUPPORTED",
    "TIMEOUT",
    "RADIO_FAILURE",
    "RATE_UNACHIEVABLE",
    "INTERNAL",
    "NOT_FOUND",
];

#[derive(Clone, serde::Serialize)]
pub struct EventFrame {
    pub seq: u16,
    pub opcode: u8,
    pub payload: Vec<u8>,
}

#[derive(serde::Serialize)]
pub struct Reply {
    pub payload: Vec<u8>,
    pub events: Vec<EventFrame>,
}

fn crc16(data: &[u8]) -> u16 {
    let mut crc = 0xffffu16;
    for b in data {
        crc ^= (*b as u16) << 8;
        for _ in 0..8 {
            crc = if crc & 0x8000 != 0 {
                (crc << 1) ^ 0x1021
            } else {
                crc << 1
            };
        }
    }
    crc
}

fn cobs_encode(input: &[u8]) -> Vec<u8> {
    let mut out = vec![0];
    let mut code_idx = 0;
    let mut code = 1u8;
    for b in input {
        if *b == 0 {
            out[code_idx] = code;
            code_idx = out.len();
            out.push(0);
            code = 1;
        } else {
            out.push(*b);
            code += 1;
            if code == 0xff {
                out[code_idx] = code;
                code_idx = out.len();
                out.push(0);
                code = 1;
            }
        }
    }
    out[code_idx] = code;
    out.push(0);
    out
}

fn cobs_decode(input: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < input.len() {
        let code = *input.get(i)? as usize;
        if code == 0 {
            return None;
        }
        i += 1;
        let end = i.checked_add(code - 1)?;
        if end > input.len() {
            return None;
        }
        out.extend_from_slice(&input[i..end]);
        i = end;
        if code != 0xff && i < input.len() {
            out.push(0);
        }
    }
    Some(out)
}

fn frame(kind: u8, seq: u16, op: u8, flags: u8, payload: &[u8]) -> Vec<u8> {
    let mut d = vec![1, kind];
    d.extend(seq.to_le_bytes());
    d.extend([op, flags]);
    d.extend((payload.len() as u16).to_le_bytes());
    d.extend(payload);
    let c = crc16(&d);
    d.extend(c.to_le_bytes());
    cobs_encode(&d)
}

fn parse_detailed(raw: &[u8]) -> Result<(u8, u16, u8, u8, Vec<u8>), String> {
    let d = cobs_decode(raw).ok_or_else(|| "COBS decode failed".to_string())?;

    if d.len() < 10 {
        return Err(format!("decoded frame too short: {} bytes", d.len()));
    }
    if d[0] != 1 {
        return Err(format!("protocol version mismatch: {}", d[0]));
    }

    let n = u16::from_le_bytes([d[6], d[7]]) as usize;
    if d.len() != 10 + n {
        return Err(format!(
            "length mismatch: decoded={} header_payload={} expected_total={}",
            d.len(),
            n,
            10 + n
        ));
    }

    let got = u16::from_le_bytes([d[d.len() - 2], d[d.len() - 1]]);
    let calculated = crc16(&d[..d.len() - 2]);
    if calculated != got {
        return Err(format!(
            "CRC mismatch: received=0x{:04X} calculated=0x{:04X}",
            got, calculated
        ));
    }

    Ok((
        d[1],
        u16::from_le_bytes([d[2], d[3]]),
        d[4],
        d[5],
        d[8..8 + n].to_vec(),
    ))
}

pub fn command(
    port: &mut Box<dyn SerialPort>,
    app: &AppHandle,
    seq: u16,
    opcode: u8,
    payload: &[u8],
    encoded: &mut Vec<u8>,
) -> Result<Reply, String> {
    let packet = frame(1, seq, opcode, 0, payload);
    let timeout = match opcode {
        0x11 | 0x12 | 0x14 | 0x2A | 0x40 | 0x41 => Duration::from_secs(5),
        _ => Duration::from_millis(1800),
    };

    port.write_all(&packet)
        .map_err(|e| format!("USB write failed: {e}"))?;
    port.flush().map_err(|e| format!("USB flush failed: {e}"))?;

    let start = Instant::now();
    let mut events = Vec::new();
    let mut buf = [0u8; 512];

    while start.elapsed() < timeout {
        match port.read(&mut buf) {
            Ok(n) => {
                let mut response: Option<Vec<u8>> = None;
                let mut response_error: Option<String> = None;

                for b in &buf[..n] {
                    if *b == 0 {
                        if !encoded.is_empty() {
                            match parse_detailed(encoded) {
                                Ok((kind, s, op, flags, p)) => {
                                    if kind == 2 && s == seq && op == opcode {
                                        if flags != 0 {
                                            let name = ERRORS
                                                .get(flags as usize)
                                                .unwrap_or(&"UNKNOWN_ERROR");
                                            response_error = Some(format!(
                                                "{} ({} / 0x{:02X})",
                                                name, flags, opcode
                                            ));
                                            eprintln!(
                                                "[PROTO ERROR] device rejected seq={} opcode=0x{:02X} with status={} ({})",
                                                s, op, flags, name
                                            );
                                        } else {
                                            response = Some(p);
                                        }
                                    } else if kind == 3 {
                                        let ev = EventFrame {
                                            seq: s,
                                            opcode: op,
                                            payload: p,
                                        };
                                        let _ = app.emit("uwb-event", ev.clone());
                                        events.push(ev);
                                    } else {
                                        eprintln!(
                                            "[PROTO WARN] ignoring unmatched frame: expected kind=2 seq={} opcode=0x{:02X}, received kind={} seq={} opcode=0x{:02X}",
                                            seq, opcode, kind, s, op
                                        );
                                    }
                                }
                                Err(reason) => {
                                    eprintln!("[PROTO WARN] discarding invalid frame: {reason}");
                                }
                            }
                        }
                        encoded.clear();
                    } else if encoded.len() < 1024 {
                        encoded.push(*b);
                    } else {
                        eprintln!(
                            "[PROTO WARN] encoded frame exceeded 1024 bytes; discarding buffered data"
                        );
                        encoded.clear();
                    }
                }

                if let Some(e) = response_error {
                    return Err(e);
                }
                if let Some(p) = response {
                    return Ok(Reply { payload: p, events });
                }
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => {
                eprintln!("[PROTO ERROR] USB read failed: {e}");
                return Err(format!("USB read failed: {e}"));
            }
        }
    }

    eprintln!(
        "[PROTO ERROR] timed out waiting for seq={} opcode=0x{:02X} after {}ms ({} partial RX byte(s))",
        seq,
        opcode,
        start.elapsed().as_millis(),
        encoded.len()
    );
    Err(format!("Timed out waiting for opcode 0x{opcode:02X}"))
}
