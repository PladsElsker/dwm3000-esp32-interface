<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue';
import { invoke } from '@tauri-apps/api/core';
import { listen, type UnlistenFn } from '@tauri-apps/api/event';
import { Activity, Antenna, ArrowDownToLine, ArrowUpFromLine, BadgeCheck, Bluetooth, Cable, ChartNoAxesCombined, ChevronDown, CircleAlert, CircleHelp, Clock3, Cpu, Database, Download, Gauge, History, Inbox, LayoutDashboard, Link2, ListChecks, Plug, Power, RefreshCw, RotateCcw, Save, Search, Send, Settings2, ShieldCheck, Terminal, Trash2, Unplug, Waves, Zap } from 'lucide-vue-next';

type Port = { name: string; description: string };
type FrameEvent = { seq: number; opcode: number; payload: number[] };
type BleConfig = {
    mode: number; advertisingIntervalMs: number; scanIntervalMs: number; scanWindowMs: number;
    retryIntervalMs: number; connectionTimeoutMs: number;
    maxRetries: number; rxMessageTimeoutMs: number; autoStart: boolean; peers: number[];
};
type BlePeerStatus = {
    peerId: number; state: number; mode: number; enabled: boolean; advertising: boolean;
    scanning: boolean; connected: boolean; mtu: number;
    lastFailureReason: number; lastFailureStage: number; lastFailureDetail: number;
};
type BleMessage = {
    peerId: number; messageId: number; length: number; time: number;
    state: string; text: string; hex: string; error?: string;
};
const defaultBleConfig = (): BleConfig => ({
    mode: 0, advertisingIntervalMs: 160, scanIntervalMs: 160, scanWindowMs: 80,
    autoStart: false, retryIntervalMs: 1000, connectionTimeoutMs: 5000,
    maxRetries: 1, rxMessageTimeoutMs: 1000, peers: []
});
const bleDraft = reactive<BleConfig>(defaultBleConfig());
const ble = reactive({
    supported: false,
    runtime: { mode: 0, enabled: false, connected: false, connectedPeerId: 0, advertising: false, scanning: false, mtu: 23 },
    runtimeConfig: defaultBleConfig(),
    savedConfig: defaultBleConfig(),
    peerStatuses: [] as BlePeerStatus[],
    discoveredPeers: [] as number[],
    inbox: [] as BleMessage[],
    outgoing: [] as BleMessage[],
    loading: false,
    saving: false,
    sending: false,
    action: ''
});
const blePeersInput = ref('');
const blePeerTokens = computed(() => blePeersInput.value.split(',').filter(token => token.trim().length > 0));
const bleVisiblePeerIds = computed(() => [...new Set([
    ...ble.runtimeConfig.peers,
    ...ble.discoveredPeers,
    ...ble.peerStatuses.map(peer => peer.peerId),
    ...(ble.runtime.connectedPeerId ? [ble.runtime.connectedPeerId] : [])
])].sort((a, b) => a - b));
const bleConnectedCount = computed(() => ble.peerStatuses.filter(peer => peer.connected).length);
const bleMessagePeerId = ref('');
const bleMessageText = ref('');
const bleMessageByteLength = computed(() => new TextEncoder().encode(bleMessageText.value).length);
const retrievingBleMessages = new Set<string>();
const connected = ref(false), connecting = ref(false), ports = ref<Port[]>([]), selectedPort = ref(''), active = ref('overview'), busy = ref(false), savingConfig = ref(false), savingStep = ref(''), lastRefresh = ref('—'), error = ref(''), notice = ref(''), events = ref<string[]>([]), measurements = ref<any[]>([]), eventCount = ref(0);

type CalJobResult = { target: number; status: number; mm: number | null };
const CAL_WINDOW_SIZE = 100;
const DEFAULT_ANTENNA_DELAY = 16385;
// One DW3000 device-time unit converted to one-way propagation distance.
const DW3000_TICK_METERS = 299792458 / (499.2e6 * 128);
const calibration = reactive({
    phase: 'idle' as 'idle'|'starting'|'live'|'saving'|'resetting'|'error',
    active: false,
    processing: false,
    operationBusy: false,
    stopRequested: false,
    referenceId: '2',
    originalRole: 0,
    requestedDelay: 16385,
    delayPending: false,
    appliedTx: 16385,
    appliedRx: 16385,
    savedTx: 16385,
    savedRx: 16385,
    averageMm: null as number | null,
    latestMm: null as number | null,
    windowValues: [] as number[],
    validSamples: 0,
    failedSamples: 0,
    connectionUnstable: false,
    connectionWarning: '',
    error: '',
    message: '',
    isSaved: true,
    activeJobId: 0,
});
const calibrationDelayInput = ref<string | number>(String(calibration.requestedDelay));
// Increasing the programmed TX/RX delay moves the adjusted TX stamps later
// and RX stamps earlier. Show the nominal range shift relative to the default.
const calibrationRangeShiftMeters = computed(() =>
    (DEFAULT_ANTENNA_DELAY - calibration.requestedDelay) * DW3000_TICK_METERS
);
watch(() => calibration.requestedDelay, value => {
    calibrationDelayInput.value = String(value);
});
let calibrationLoopPromise: Promise<void> | null = null;
const calibrationWaiters = new Map<number, { resolve: (v: CalJobResult) => void; reject: (e: Error) => void; timer: number }>();
const calibrationJobResults = new Map<number, CalJobResult>();
let poller: number | undefined, unlisten: UnlistenFn | undefined, polling = false, missingPortChecks = 0, losingConnection = false;
let lastBleRefreshAt = 0;
let commandQueue: Promise<void> = Promise.resolve();
const device = reactive<any>({ info: null, status: null, stats: null,
    // config/savedTargets always describe NVS; runtimeConfig/schedule/targets describe RAM.
    config: { deviceId: 1, panId: 0xDECA, channel: 5, tx: 16385, rx: 16385, period: 100, timeout: 120, retries: 1, role: 0, autoStart: false },
    savedTargets: [] as number[], runtimeConfig: null, schedule: null, targets: [] as number[], diagnostics: null, lastError: null });
const draft = reactive({ deviceId: 1, panId: 'DECA', channel: 5, tx: 16385, rx: 16385, period: 100, timeout: 120, retries: 1, role: 0, targets: '', autoStart: false });
type RadioTrace = { uptimeMs:number; statusBefore:number; statusAfter:number; delayedTimeReg:number; peer:number; jobId:number; event:number; radioState:number; func:number; radioSeq:number; delayed:boolean; retriesUsed:number; rc:number; reserved:number };
type RadioTraceDetail = { uptimeMs:number; statusBefore:number; statusAfter:number; sysStateBefore:number; sysStateAfter:number; sysTimeBefore:number; delayedTimeReg:number; scheduledTxTimestampLow:number; peer:number; jobId:number; event:number; state:number; func:number; radioSeq:number; delayed:boolean; retriesUsed:number; rc:number; failureHint:number };
const radioTrace = ref<RadioTrace[]>([]);
const traceSupported = ref<boolean | null>(null);
const radioTraceDetail = ref<RadioTraceDetail[]>([]);
const traceDetailSupported = ref<boolean | null>(null);
const traceDetailFetchedAt = ref('');
const traceDetailError = ref('');
const traceDetailBusy = ref(false);
const traceFetchedAt = ref('');
const traceError = ref('');
const traceBusy = ref(false);
const traceEventName = (n:number) => ({1:'dwt_starttx() failed',2:'dwt_rxenable() failed',3:'TXFRS completion timeout',4:'DW3000 RX error bits'} as Record<number,string>)[n] ?? `Unknown event ${n}`;
const radioFunctionName = (n:number) => ({0xe0:'POLL',0xe1:'RESP',0xe2:'FINAL',0xe3:'REPORT'} as Record<number,string>)[n] ?? hex(n,2);
const radioStateName = (n:number) => ({0:'READY',1:'RX_LISTEN',2:'WAIT_RESP',3:'WAIT_FINAL'} as Record<number,string>)[n] ?? `State ${n}`;
function decodeRadioTrace(p:number[]) {
  if (!p.length) throw new Error('GET_RADIO_TRACE returned an empty payload.');
  const count = p[0];
  if (count > 8 || p.length !== 1 + count * 28) throw new Error(`Invalid radio trace payload: count=${count}, bytes=${p.length}. Expected ${1 + count * 28}.`);
  return Array.from({length:count},(_,i) => {
    const o=1+i*28;
    return { uptimeMs:get32(p,o), statusBefore:get32(p,o+4), statusAfter:get32(p,o+8), delayedTimeReg:get32(p,o+12), peer:get16(p,o+16), jobId:get16(p,o+18), event:p[o+20], radioState:p[o+21], func:p[o+22], radioSeq:p[o+23], delayed:p[o+24]!==0, retriesUsed:p[o+25], rc:p[o+26], reserved:p[o+27] } as RadioTrace;
  });
}
async function loadRadioTrace() {
  traceBusy.value=true; traceError.value='';
  try {
    const p=await cmd(0x36);
    radioTrace.value=decodeRadioTrace(p); traceSupported.value=true; traceFetchedAt.value=new Date().toLocaleString();
    log(`GET_RADIO_TRACE · ${radioTrace.value.length} record(s)`);
    if (!radioTrace.value.length) notice.value='Radio trace read successfully; no failure records are retained.';
  } catch(e:any) {
    const message=String(e); traceError.value=message;
    if (/UNKNOWN_COMMAND|UNKNOWN_COMMAND \(2|0x02/i.test(message)) traceSupported.value=false;
    log(`GET_RADIO_TRACE failed · ${message}`);
  } finally { traceBusy.value=false; }
}
function decodeRadioTraceDetail(p:number[]) {
  if (!p.length) throw new Error('GET_RADIO_TRACE_DETAIL returned an empty payload.');
  const count=p[0];
  if (count>8 || p.length!==1+count*44) throw new Error(`Invalid detailed trace payload: count=${count}, bytes=${p.length}. Expected ${1+count*44}.`);
  return Array.from({length:count},(_,i)=>{
    const o=1+i*44;
    return {
      uptimeMs:get32(p,o), statusBefore:get32(p,o+4), statusAfter:get32(p,o+8),
      sysStateBefore:get32(p,o+12), sysStateAfter:get32(p,o+16), sysTimeBefore:get32(p,o+20),
      delayedTimeReg:get32(p,o+24), scheduledTxTimestampLow:get32(p,o+28),
      peer:get16(p,o+32), jobId:get16(p,o+34), event:p[o+36], state:p[o+37],
      func:p[o+38], radioSeq:p[o+39], delayed:p[o+40]!==0, retriesUsed:p[o+41],
      rc:p[o+42], failureHint:p[o+43]
    } as RadioTraceDetail;
  });
}
async function loadRadioTraceDetail() {
  traceDetailBusy.value=true; traceDetailError.value='';
  try {
    const p=await cmd(0x37);
    radioTraceDetail.value=decodeRadioTraceDetail(p);
    traceDetailSupported.value=true;
    traceDetailFetchedAt.value=new Date().toLocaleString();
    log(`GET_RADIO_TRACE_DETAIL · ${radioTraceDetail.value.length} record(s)`);
    if (!radioTraceDetail.value.length) notice.value='Detailed TX trace read successfully; no failed TX-start records are retained.';
  } catch(e:any) {
    const message=String(e); traceDetailError.value=message;
    if (/UNKNOWN_COMMAND|UNKNOWN_COMMAND \(2|0x02/i.test(message)) traceDetailSupported.value=false;
    log(`GET_RADIO_TRACE_DETAIL failed · ${message}`);
  } finally { traceDetailBusy.value=false; }
}
const failureHintName=(n:number)=>({0:'Unknown / inconclusive',1:'HPDWARN observed after call',2:'TXERR state observed after call'} as Record<number,string>)[n] ?? `Unknown hint ${n}`;
function buildDebugReport() {
  return { reportFormat:'D3KM device debug report', reportVersion:3, protocolSpecVersion:'1.4', capturedAt:new Date().toISOString(), port:selectedPort.value, appProtocolVersion:1, device:{ info:device.info, status:device.status, savedConfig:device.config, savedTargets:device.savedTargets, runtimeConfig:device.runtimeConfig, runtimeSchedule:device.schedule, runtimeTargets:device.targets, stats:device.stats, diagnostics:device.diagnostics, lastError:device.lastError }, bluetooth:{ supported:ble.supported, runtime:{ ...ble.runtime }, runtimeConfig:cloneBleConfig(ble.runtimeConfig), savedConfig:cloneBleConfig(ble.savedConfig), peerStatuses:ble.peerStatuses.map(item => ({ ...item })), discoveredPeers:[...ble.discoveredPeers], receivedCount:ble.inbox.length, sentCount:ble.outgoing.length, incomingMessageMetadata:ble.inbox.map(({ peerId, messageId, length, time, state }) => ({ peerId, messageId, length, time, state })), outgoingMessageMetadata:ble.outgoing.map(({ peerId, messageId, length, time, state, error }) => ({ peerId, messageId, length, time, state, error })) }, radioTrace:{ supported:traceSupported.value, fetchedAt:traceFetchedAt.value, records:radioTrace.value }, radioTraceDetail:{ supported:traceDetailSupported.value, fetchedAt:traceDetailFetchedAt.value, records:radioTraceDetail.value }, antennaCalibration:{ phase:calibration.phase, referenceId:calibration.referenceId, requestedDelay:calibration.requestedDelay, appliedTx:calibration.appliedTx, appliedRx:calibration.appliedRx, savedTx:calibration.savedTx, savedRx:calibration.savedRx, averageMm:calibration.averageMm, latestMm:calibration.latestMm, windowSize:CAL_WINDOW_SIZE, validSamples:calibration.validSamples, failedSamples:calibration.failedSamples, isSaved:calibration.isSaved }, recentEvents:events.value.slice(0,40), recentMeasurements:measurements.value.slice(0,20) };
}
async function copyDebugReport() {
  try { await navigator.clipboard.writeText(JSON.stringify(buildDebugReport(),null,2)); notice.value='Debug report copied to clipboard'; log('Debug report copied'); }
  catch(e:any) { error.value=`Could not copy report: ${String(e)}`; }
}
function downloadDebugReport() {
  const blob=new Blob([JSON.stringify(buildDebugReport(),null,2)],{type:'application/json'});
  const url=URL.createObjectURL(blob); const a=document.createElement('a'); a.href=url; a.download=`d3km-debug-node-${device.info?.id ?? 'unknown'}-${new Date().toISOString().replace(/[:.]/g,'-')}.json`; a.click(); URL.revokeObjectURL(url);
}

const roleName = computed(() => ['Idle', 'Initiator', 'Responder'][device.status?.role ?? 0] ?? 'Unknown');
const health = computed(() => !connected.value ? 'Disconnected' : !device.status ? 'Connected' : device.status.radio_ready ? 'Healthy' : 'Radio unavailable');
const bleDirty = computed(() => ble.supported && (JSON.stringify(bleDraft) !== JSON.stringify(ble.savedConfig) || blePeersInput.value !== ble.savedConfig.peers.join(', ')));
const dirty = computed(() => Number(draft.deviceId) !== device.config.deviceId || parseInt(draft.panId, 16) !== device.config.panId || draft.channel !== device.config.channel || draft.tx !== device.config.tx || draft.rx !== device.config.rx || draft.period !== device.config.period || draft.timeout !== device.config.timeout || draft.retries !== device.config.retries || draft.role !== device.config.role || draft.autoStart !== device.config.autoStart || draft.targets !== device.savedTargets.join(', ') || bleDirty.value);
const hex = (n: number, w = 4) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(w, '0');
const u16 = (n: number) => [n & 255, (n >> 8) & 255], u32 = (n: number) => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
const get16 = (p: number[], i = 0) => p[i] | (p[i + 1] << 8), get32 = (p: number[], i = 0) => (p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (p[i + 3] << 24)) >>> 0;
const get64 = (p: number[], i = 0) => BigInt(get32(p, i)) + (BigInt(get32(p, i + 4)) << 32n);
function decodeBleConfigExtension(p: number[], offset: number): BleConfig {
    if (p.length < offset + 16) throw new Error(`BLE configuration extension is truncated (${p.length - offset} bytes).`);
    const peerCount = p[offset + 15];
    if (peerCount > 8 || p.length !== offset + 16 + peerCount * 2) {
        throw new Error(`Invalid BLE configuration extension: peer count ${peerCount}, length ${p.length - offset}.`);
    }
    const config: BleConfig = {
        mode: p[offset], advertisingIntervalMs: get16(p, offset + 1), scanIntervalMs: get16(p, offset + 3),
        scanWindowMs: get16(p, offset + 5),
        autoStart: p[offset + 7] === 1,
        retryIntervalMs: get16(p, offset + 8), connectionTimeoutMs: get16(p, offset + 10),
        maxRetries: p[offset + 12], rxMessageTimeoutMs: get16(p, offset + 13),
        peers: Array.from({ length: peerCount }, (_, i) => get16(p, offset + 16 + i * 2))
    };
    if (config.mode > 2 || p[offset + 7] > 1 || config.maxRetries > 5 ||
        config.advertisingIntervalMs < 20 || config.advertisingIntervalMs > 10240 ||
        config.scanIntervalMs < 10 || config.scanIntervalMs > 10240 ||
        config.scanWindowMs < 10 || config.scanWindowMs > config.scanIntervalMs ||
        config.retryIntervalMs < 100 || config.retryIntervalMs > 60000 ||
        config.connectionTimeoutMs < 100 || config.connectionTimeoutMs > 30000 ||
        config.rxMessageTimeoutMs < 100 || config.rxMessageTimeoutMs > 2000) {
        throw new Error('Device returned BLE settings outside the supported protocol limits.');
    }
    if (config.peers.some((id, i) => id < 1 || id > 65534 || config.peers.indexOf(id) !== i)) {
        throw new Error('Device returned an invalid or duplicate BLE peer ID.');
    }
    return config;
}
function encodeBleConfigExtension(c: BleConfig) {
    return [
        c.mode, ...u16(c.advertisingIntervalMs), ...u16(c.scanIntervalMs), ...u16(c.scanWindowMs),
        c.autoStart ? 1 : 0, ...u16(c.retryIntervalMs), ...u16(c.connectionTimeoutMs),
        c.maxRetries, ...u16(c.rxMessageTimeoutMs), c.peers.length, ...c.peers.flatMap(u16)
    ];
}
function cloneBleConfig(c: BleConfig): BleConfig {
    return { ...c, peers: [...c.peers] };
}
function decodeSavedConfig(p: number[]) {
    if (p.length < 19) throw new Error(`GET_SAVED_CONFIG payload too short: ${p.length} bytes.`);
    const count = p[17];
    const baseLength = 19 + count * 2;
    if (count > 16 || p.length < baseLength) throw new Error(`Invalid GET_SAVED_CONFIG payload: target count ${count}, length ${p.length}.`);
    const bleConfigAvailable = p.length > baseLength;
    const bleConfig = bleConfigAvailable ? decodeBleConfigExtension(p, baseLength) : defaultBleConfig();
    return {
        deviceId: get16(p, 0), panId: get16(p, 2), channel: p[4], tx: get16(p, 5), rx: get16(p, 7),
        period: get32(p, 9), timeout: get16(p, 13), retries: p[15], role: p[16],
        targets: Array.from({ length: count }, (_, i) => get16(p, 18 + i * 2)),
        autoStart: p[18 + count * 2] === 1, bleConfig, bleConfigAvailable
    };
}
function readString(p: number[]) { return String.fromCharCode(...p) }
async function cmd(op: number, payload: number[] = []) {
    if (!connected.value) throw new Error('Connect a device first');
    const task = commandQueue.then(() => invoke<{ payload: number[]; events: FrameEvent[] }>('send_command', { opcode: op, payload }));
    commandQueue = task.then(() => undefined, () => undefined);
    try {
        return (await task).payload;
    } catch (e: any) {
        if (isUsbDisconnectError(e)) void handleUsbLoss('USB connection lost. Reconnect the device to continue.');
        throw e;
    }
}
function log(message: string) { events.value.unshift(`${new Date().toLocaleTimeString()}  ${message}`); events.value = events.value.slice(0, 120) }
function applyBleStatusPayload(p: number[]) {
    if (p.length < 27) {
        ble.supported = false;
        Object.assign(ble.runtime, { mode: 0, enabled: false, connected: false, connectedPeerId: 0, advertising: false, scanning: false, mtu: 23 });
        return;
    }
    ble.supported = true;
    Object.assign(ble.runtime, {
        mode: p[18], enabled: p[19] === 1, connected: p[20] === 1, connectedPeerId: get16(p, 21),
        advertising: p[23] === 1, scanning: p[24] === 1, mtu: get16(p, 25)
    });
    if (ble.runtime.connected && ble.runtime.connectedPeerId) bleMessagePeerId.value = String(ble.runtime.connectedPeerId);
}
function decodeBlePeerStatus(p: number[]): BlePeerStatus {
    if (p.length !== 10 && p.length !== 14) throw new Error(`GET_PEER_STATUS returned ${p.length} bytes; expected 10 or 14.`);
    const rawFailureDetail = p.length >= 14 ? get16(p, 12) : 0;
    return {
        peerId: get16(p, 6), state: p[0], mode: p[1], enabled: p[2] === 1,
        advertising: p[3] === 1, scanning: p[4] === 1, connected: p[5] === 1, mtu: get16(p, 8),
        lastFailureReason: p.length >= 14 ? p[10] : 0,
        lastFailureStage: p.length >= 14 ? p[11] : 0,
        lastFailureDetail: rawFailureDetail >= 0x8000 ? rawFailureDetail - 0x10000 : rawFailureDetail
    };
}
function updateBlePeerStatus(status: BlePeerStatus) {
    const index = ble.peerStatuses.findIndex(item => item.peerId === status.peerId);
    if (index >= 0) ble.peerStatuses.splice(index, 1, status);
    else ble.peerStatuses.push(status);
}
function getBlePeerStatus(peerId: number): BlePeerStatus | undefined {
    return ble.peerStatuses.find(item => item.peerId === peerId);
}
async function refreshBlePeerStatus(peerId: number, silent = false) {
    if (!connected.value || !ble.supported || !Number.isInteger(peerId) || peerId < 1 || peerId > 65534) return null;
    try {
        const payload = await cmd(0x56, u16(peerId));
        const status = decodeBlePeerStatus(payload);
        updateBlePeerStatus(status);
        return status;
    } catch (e: any) {
        if (!silent) error.value = `Could not read Bluetooth peer ${peerId}: ${String(e)}`;
        return null;
    }
}
async function refreshBluetooth() {
    if (!connected.value || !ble.supported) return;
    ble.loading = true;
    try {
        const [statusPayload, peersPayload] = await Promise.all([cmd(0x03), cmd(0x51)]);
        applyBleStatusPayload(statusPayload);
        if (!peersPayload.length || peersPayload[0] > 8 || peersPayload.length !== 1 + peersPayload[0] * 2) {
            throw new Error('GET_PEERS returned an invalid peer-list payload.');
        }
        const peers = Array.from({ length: peersPayload[0] }, (_, i) => get16(peersPayload, 1 + i * 2));
        ble.runtimeConfig.peers = peers;
        const ids = [...new Set([...peers, ...ble.discoveredPeers, ...(ble.runtime.connectedPeerId ? [ble.runtime.connectedPeerId] : [])])];
        // Keep the last known statuses visible while the per-peer reads complete.
        ble.peerStatuses = ble.peerStatuses.filter(peer => ids.includes(peer.peerId));
        for (const id of ids) await refreshBlePeerStatus(id, true);
        log(`Bluetooth status refreshed · ${peers.length} configured peer(s)`);
    } catch (e: any) {
        error.value = `Bluetooth refresh failed: ${String(e)}`;
    } finally {
        ble.loading = false;
    }
}
function parseBlePeerIds(input: string): number[] {
    const trimmed = input.trim();
    if (!trimmed) return [];
    const tokens = trimmed.split(',').map(token => token.trim());
    if (tokens.some(token => !/^\d+$/.test(token))) {
        throw new Error('Bluetooth peer IDs must be comma-separated decimal integers.');
    }
    const peers = tokens.map(Number);
    if (peers.length > 8) throw new Error('The firmware supports a maximum of 8 Bluetooth peers.');
    if (peers.some(id => !Number.isInteger(id) || id < 1 || id > 65534)) {
        throw new Error('Bluetooth peer IDs must be integers from 1 to 65534.');
    }
    if (peers.includes(Number(draft.deviceId))) {
        throw new Error('A node cannot be configured as its own Bluetooth peer.');
    }
    if (new Set(peers).size !== peers.length) {
        throw new Error('Bluetooth peer IDs must be unique; remove duplicate IDs.');
    }
    return peers;
}
function validateBleConfig(c: BleConfig) {
    if (!Number.isInteger(c.mode) || c.mode < 0 || c.mode > 2) throw new Error('Select a valid Bluetooth mode.');
    if (c.autoStart && c.mode === 0) throw new Error('Bluetooth auto-start requires HUB or PERIPHERAL mode.');
    if (!Number.isInteger(c.advertisingIntervalMs) || c.advertisingIntervalMs < 20 || c.advertisingIntervalMs > 10240) throw new Error('Bluetooth advertising interval must be 20–10240 ms.');
    if (!Number.isInteger(c.scanIntervalMs) || c.scanIntervalMs < 10 || c.scanIntervalMs > 10240) throw new Error('Bluetooth scan interval must be 10–10240 ms.');
    if (!Number.isInteger(c.scanWindowMs) || c.scanWindowMs < 10 || c.scanWindowMs > c.scanIntervalMs) throw new Error('Bluetooth scan window must be 10 ms or greater and cannot exceed the scan interval.');
    if (!Number.isInteger(c.retryIntervalMs) || c.retryIntervalMs < 100 || c.retryIntervalMs > 60000) throw new Error('Bluetooth retry interval must be 100–60000 ms.');
    if (!Number.isInteger(c.connectionTimeoutMs) || c.connectionTimeoutMs < 100 || c.connectionTimeoutMs > 30000) throw new Error('Bluetooth connection timeout must be 100–30000 ms.');
    if (!Number.isInteger(c.maxRetries) || c.maxRetries < 0 || c.maxRetries > 5) throw new Error('Bluetooth retries must be 0–5.');
    if (!Number.isInteger(c.rxMessageTimeoutMs) || c.rxMessageTimeoutMs < 100 || c.rxMessageTimeoutMs > 2000) throw new Error('Bluetooth receive timeout must be 100–2000 ms.');
    if (c.peers.length > 8 || c.peers.some(id => !Number.isInteger(id) || id < 1 || id > 65534 || id === Number(draft.deviceId)) || new Set(c.peers).size !== c.peers.length) {
        throw new Error('Bluetooth peers must be up to 8 unique IDs from 1 to 65534, excluding this device ID.');
    }
}
async function startBle() {
    if (!ble.supported) { error.value = 'Bluetooth controls require firmware with BLE protocol support.'; return; }
    if (ble.runtimeConfig.mode === 0) { error.value = 'Set Bluetooth mode to HUB or PERIPHERAL in Configuration before starting BLE.'; return; }
    ble.action = 'start';
    try {
        await cmd(0x52);
        await refreshStatus();
        notice.value = ble.runtimeConfig.mode === 1
            ? (ble.runtimeConfig.peers.length ? 'Bluetooth enabled; automatically discovering and connecting to allowed peers.' : 'Bluetooth enabled in open mode; automatically discovering peers.')
            : 'Bluetooth enabled; advertising and accepting permitted peers.';
        log(`Bluetooth started · ${bleModeName(ble.runtimeConfig.mode)}`);
    } catch (e: any) { error.value = String(e); }
    finally { ble.action = ''; }
}
async function stopBle() {
    if (!ble.supported) return;
    ble.action = 'stop';
    try {
        await cmd(0x53);
        await refreshStatus();
        notice.value = 'Bluetooth disabled for the current boot. The saved auto-start preference is unchanged.';
        log('Bluetooth disabled · boot auto-start preference unchanged');
    } catch (e: any) { error.value = String(e); }
    finally { ble.action = ''; }
}
async function toggleBle() {
    if (ble.action) return;
    if (ble.runtime.enabled) await stopBle();
    else await startBle();
}
async function sendBleMessage() {
    if (!ble.supported || ble.sending) return;
    const peerId = Number(bleMessagePeerId.value || ble.runtime.connectedPeerId);
    if (!Number.isInteger(peerId) || peerId < 1 || peerId > 65534) { error.value = 'Enter a valid Bluetooth peer ID.'; return; }
    if (!getBlePeerStatus(peerId)?.connected) { error.value = `Bluetooth is not connected to peer ${peerId}.`; return; }
    const bytes = Array.from(new TextEncoder().encode(bleMessageText.value));
    if (!bytes.length) { error.value = 'Enter a message to send.'; return; }
    if (bytes.length > 496) { error.value = `Message is ${bytes.length} bytes; the maximum is 496 bytes.`; return; }
    ble.sending = true;
    try {
        const response = await cmd(0x57, [...u16(peerId), ...u16(bytes.length), ...bytes]);
        if (response.length !== 2) throw new Error(`SEND_TO_PEER returned ${response.length} bytes; expected a 2-byte message ID.`);
        const messageId = get16(response);
        ble.outgoing.unshift({ peerId, messageId, length: bytes.length, time: Date.now(), state: 'Accepted · awaiting remote retrieval', text: bleMessageText.value, hex: formatHexBytes(bytes) });
        ble.outgoing = ble.outgoing.slice(0, 40);
        log(`Bluetooth message queued · peer ${peerId} · message ${messageId} · ${bytes.length} bytes`);
        notice.value = `Message ${messageId} accepted for transmission. Delivery is confirmed when the remote host retrieves it.`;
        bleMessageText.value = '';
    } catch (e: any) { error.value = String(e); }
    finally { ble.sending = false; }
}
async function sendBleTestMessage() {
    bleMessageText.value = `D3KM BLE test from node ${device.info?.id ?? 'unknown'} at ${new Date().toISOString()}`;
    await sendBleMessage();
}
function clearBleMessages() {
    ble.inbox = [];
    ble.outgoing = [];
}
function blePeerStateName(state: number) {
    return ({
        0: 'Unknown', 1: 'Configured', 2: 'Discovered', 3: 'Connected',
        4: 'Connecting', 5: 'Handshaking', 6: 'Retry pending'
    } as Record<number, string>)[state] ?? `State ${state}`;
}
function bleModeName(mode: number) {
    return ({ 0: 'Idle', 1: 'HUB / Central', 2: 'Peripheral' } as Record<number, string>)[mode] ?? `Mode ${mode}`;
}
function bleConnectionStageName(stage: number) {
    return ({
        1: 'Stopping scan', 2: 'Allocating client', 3: 'Starting GAP connection',
        4: 'GAP connection result', 5: 'Finding GATT service',
        6: 'Validating GATT characteristics', 7: 'Checking BLE protocol version',
        8: 'Checking peer identity', 9: 'Writing local identity',
        10: 'Subscribing to notifications', 11: 'Peripheral identity validation',
        12: 'Peripheral handshake timeout'
    } as Record<number, string>)[stage] ?? `Connection stage ${stage}`;
}
function bleConnectionDetailText(stage: number, detail: number) {
    if (detail === -1 || (detail === 0 && [5, 6, 9, 10, 12].includes(stage))) return 'detail unavailable';
    const hexDetail = `0x${(detail & 0xffff).toString(16).toUpperCase().padStart(4, '0')}`;
    if ([3, 4].includes(stage)) return `NimBLE status ${hexDetail}`;
    if (stage === 8) return `reported peer ID ${hexDetail}`;
    if (stage === 11) return `validation code ${detail}`;
    return `detail ${hexDetail}`;
}
function bleConnectionFailureText(status?: BlePeerStatus) {
    if (!status?.lastFailureReason) return '';
    const reason = ({
        1: 'Local connection setup failed', 3: 'Peer identity rejected',
        5: 'GATT or identity handshake failed', 6: 'GAP connection failed'
    } as Record<number, string>)[status.lastFailureReason] ?? `Failure ${status.lastFailureReason}`;
    const detail = bleConnectionDetailText(status.lastFailureStage, status.lastFailureDetail);
    return `${reason} · ${bleConnectionStageName(status.lastFailureStage)} · ${detail}`;
}
function bleErrorName(code: number, direction = 0) {
    const names: Record<number, string> = {
        1: direction === 0 ? 'Remote receive queue full or message consumption timed out' : 'Remote receive queue full',
        2: 'Remote receiver busy', 3: 'Message too large', 4: 'Invalid fragment',
        5: 'Fragment metadata mismatch', 6: 'Unsupported BLE protocol version',
        7: 'BLE transport write failed', 8: 'Fragment acknowledgement retry limit exhausted',
        9: 'Delivery outcome unknown (final result acknowledgement timed out)',
        11: 'Invalid final message result'
    };
    return names[code] ?? `BLE error ${code}`;
}
function formatHexBytes(bytes: number[], limit = 48) {
    const shown = bytes.slice(0, limit).map(b => b.toString(16).padStart(2, '0')).join(' ');
    return bytes.length > limit ? `${shown} … (${bytes.length} bytes)` : shown;
}
function dispatchCalibrationJob(jobId: number, result: CalJobResult) {
    calibrationJobResults.set(jobId, result);
    while (calibrationJobResults.size > 24) calibrationJobResults.delete(calibrationJobResults.keys().next().value!);
    const waiter = calibrationWaiters.get(jobId);
    if (waiter) {
        window.clearTimeout(waiter.timer);
        calibrationWaiters.delete(jobId);
        calibrationJobResults.delete(jobId);
        waiter.resolve(result);
    }
    if (calibration.activeJobId === jobId) calibration.activeJobId = 0;
}
function handleEvent(e: FrameEvent) {
    eventCount.value++;
    const p = e.payload;
    const names: Record<number, string> = {
        128: 'RANGE_RESULT', 129: 'RANGE_FAILED', 130: 'STATE_CHANGED', 131: 'ERROR', 132: 'HISTORY_OVERRUN', 133: 'DEVICE_READY',
        134: 'BLE_STATE_CHANGED', 135: 'BLE_PEER_FOUND', 136: 'BLE_PEER_CONNECTED', 137: 'BLE_PEER_DISCONNECTED',
        138: 'BLE_CONNECTION_FAILED', 139: 'BLE_DATA_RECEIVED', 140: 'BLE_ERROR', 141: 'BLE_DATA_SENT', 142: 'BLE_DATA_EXPIRED'
    };
    log(`${names[e.opcode] ?? 'EVENT'} · seq ${e.seq}`);
    if (e.opcode === 128 && p.length >= 30) {
        const jobId = get16(p, 0);
        const m = decodeMeasurement(p.slice(2));
        measurements.value.unshift(m);
        measurements.value = measurements.value.slice(0, 80);
        if (jobId) dispatchCalibrationJob(jobId, { target: m.target, status: m.status, mm: m.mm });
    }
    if (e.opcode === 129 && p.length >= 13) {
        const jobId = get16(p, 0);
        const target = get16(p, 2);
        const status = p[4];
        measurements.value.unshift({ target, mm: -1, status, seq: '—', time: Date.now() });
        if (jobId) dispatchCalibrationJob(jobId, { target, status, mm: null });
    }
    if (e.opcode === 134 && p.length >= 4) {
        Object.assign(ble.runtime, { enabled: p[0] === 1, mode: p[1], advertising: p[2] === 1, scanning: p[3] === 1 });
        log(`BLE ${ble.runtime.enabled ? 'enabled' : 'stopped'} · ${bleModeName(ble.runtime.mode)}`);
    }
    if (e.opcode === 135 && p.length >= 2) {
        const peerId = get16(p);
        if (!ble.discoveredPeers.includes(peerId)) ble.discoveredPeers.push(peerId);
        log(`BLE peer discovered · ${peerId}`);
        void refreshBlePeerStatus(peerId);
    }
    if (e.opcode === 136 && p.length >= 2) {
        const peerId = get16(p);
        if (!ble.discoveredPeers.includes(peerId)) ble.discoveredPeers.push(peerId);
        Object.assign(ble.runtime, { connected: true, connectedPeerId: peerId, enabled: true });
        if (!bleMessagePeerId.value) bleMessagePeerId.value = String(peerId);
        log(`BLE connected · peer ${peerId}`);
        void refreshBlePeerStatus(peerId);
        void refreshStatus();
    }
    if (e.opcode === 137 && p.length >= 3) {
        const peerId = get16(p);
        log(`BLE disconnected · peer ${peerId} · reason ${p[2]}`);
        void refreshBlePeerStatus(peerId);
        void refreshStatus();
    }
    if (e.opcode === 138 && p.length >= 3) {
        const peerId = get16(p);
        const peripheralIncomingFailure = peerId === 0 && ble.runtimeConfig.mode === 2 && p[2] === 6;
        const reason = peripheralIncomingFailure
            ? 'Incoming Peripheral connection rejected'
            : ({
                1: 'Local setup failed', 3: 'Peer identity rejected',
                5: 'GATT or identity handshake failed', 6: 'GAP connection failed'
            } as Record<number, string>)[p[2]] ?? `Failure ${p[2]}`;
        if (p.length >= 6) {
            const detailRaw = get16(p, 4);
            const detail = detailRaw >= 0x8000 ? detailRaw - 0x10000 : detailRaw;
            const stage = bleConnectionStageName(p[3]);
            const detailText = bleConnectionDetailText(p[3], detail);
            log(`BLE connection failed · ${peerId ? `peer ${peerId}` : 'incoming link'} · ${reason} · ${stage} · ${detailText}`);
        } else {
            log(`BLE connection failed · peer ${peerId || 'unknown'} · ${reason}`);
        }
        if (peerId) void refreshBlePeerStatus(peerId);
        else if (ble.runtimeConfig.mode === 2) {
            for (const id of ble.runtimeConfig.peers) void refreshBlePeerStatus(id, true);
        }
    }
    if (e.opcode === 139 && p.length >= 6) {
        const peerId = get16(p), messageId = get16(p, 2), length = get16(p, 4);
        log(`BLE message available · peer ${peerId} · message ${messageId} · ${length} bytes`);
        void retrieveBleMessage(peerId, messageId, length);
    }
    if (e.opcode === 140 && p.length >= 8) {
        const direction = p[0], peerId = get16(p, 1), messageId = get16(p, 3), fragment = p[5], code = p[6];
        const message = bleErrorName(code, direction);
        if (direction === 0) {
            const sent = ble.outgoing.find(item => item.peerId === peerId && item.messageId === messageId);
            if (sent) { sent.state = 'Failed'; sent.error = message; }
        }
        log(`BLE ${direction === 0 ? 'TX' : 'RX'} error · peer ${peerId} · message ${messageId} · fragment ${fragment === 255 ? '—' : fragment} · ${message}`);
    }
    if (e.opcode === 141 && p.length >= 6) {
        const peerId = get16(p), messageId = get16(p, 2), length = get16(p, 4);
        const sent = ble.outgoing.find(item => item.peerId === peerId && item.messageId === messageId);
        if (sent) sent.state = 'Delivered';
        log(`BLE message delivered · peer ${peerId} · message ${messageId} · ${length} bytes`);
    }
    if (e.opcode === 142 && p.length >= 4) {
        const peerId = get16(p), messageId = get16(p, 2);
        ble.inbox.unshift({ peerId, messageId, length: 0, time: Date.now(), state: 'Expired before retrieval', text: '', hex: '' });
        ble.inbox = ble.inbox.slice(0, 40);
        log(`BLE received message expired · peer ${peerId} · message ${messageId}`);
    }
}
async function retrieveBleMessage(peerId: number, messageId: number, announcedLength: number) {
    const key = `${peerId}:${messageId}`;
    if (retrievingBleMessages.has(key)) return;
    retrievingBleMessages.add(key);
    try {
        const p = await cmd(0x58, [...u16(peerId), ...u16(messageId)]);
        if (p.length < 4) throw new Error(`GET_RECEIVED_DATA returned only ${p.length} bytes.`);
        const returnedId = get16(p, 0), length = get16(p, 2);
        if (returnedId !== messageId || length !== p.length - 4 || length !== announcedLength) {
            throw new Error(`Received BLE message length/ID mismatch (event ${messageId}/${announcedLength}, response ${returnedId}/${length}).`);
        }
        const bytes = p.slice(4);
        ble.inbox.unshift({ peerId, messageId, length, time: Date.now(), state: 'Retrieved',
            text: new TextDecoder('utf-8').decode(new Uint8Array(bytes)), hex: formatHexBytes(bytes) });
        ble.inbox = ble.inbox.slice(0, 40);
        log(`BLE message retrieved · peer ${peerId} · message ${messageId} · ${length} bytes`);
    } catch (e: any) {
        log(`Could not retrieve BLE message ${messageId} from peer ${peerId}: ${String(e)}`);
        error.value = `BLE message ${messageId} could not be retrieved: ${String(e)}`;
    } finally {
        retrievingBleMessages.delete(key);
    }
}
function decodeMeasurement(p: number[]) { return { seq: get32(p), target: get16(p, 4), mono: get64(p, 6).toString(), epoch: get64(p, 14).toString(), mm: (get32(p, 22) | 0), status: p[26], quality: p[27], time: Date.now() } }
function isUsbDisconnectError(errorLike: unknown) {
    const message = String(errorLike ?? '');
    return /No USB device connected|USB (?:read|write|flush) failed|No such device|device not found|broken pipe|I\/O error|device has been disconnected|port is closed|not connected/i.test(message);
}
function clearDeviceSession() {
    for (const [jobId, waiter] of calibrationWaiters) { window.clearTimeout(waiter.timer); waiter.reject(new Error('USB session ended during calibration.')); calibrationWaiters.delete(jobId); }
    calibrationJobResults.clear();
    calibration.stopRequested = true;
    calibration.active = false; calibration.processing = false; calibration.operationBusy = false;
    calibration.activeJobId = 0; calibration.originalRole = 0; calibration.phase = 'idle'; calibrationLoopPromise = null;
    calibration.windowValues = []; calibration.averageMm = null; calibration.latestMm = null;
    calibration.connectionUnstable = false; calibration.connectionWarning = '';
    device.info = null;
    device.status = null;
    device.stats = null;
    device.config = { deviceId: 1, panId: 0xDECA, channel: 5, tx: 16385, rx: 16385, period: 100, timeout: 120, retries: 1, role: 0, autoStart: false };
    device.savedTargets = [];
    device.runtimeConfig = null;
    device.schedule = null;
    device.diagnostics = null;
    device.lastError = null;
    device.targets = [];
    ble.supported = false;
    Object.assign(ble.runtime, { mode: 0, enabled: false, connected: false, connectedPeerId: 0, advertising: false, scanning: false, mtu: 23 });
    Object.assign(ble.runtimeConfig, defaultBleConfig());
    Object.assign(ble.savedConfig, defaultBleConfig());
    Object.assign(bleDraft, defaultBleConfig());
    ble.peerStatuses = []; ble.discoveredPeers = []; ble.inbox = []; ble.outgoing = [];
    ble.loading = false; ble.saving = false; ble.sending = false; ble.action = '';
    bleMessagePeerId.value = ''; bleMessageText.value = ''; blePeersInput.value = '';
    measurements.value = [];
    events.value = [];
    eventCount.value = 0;
    Object.assign(draft, { deviceId: 1, panId: 'DECA', channel: 5, tx: 16385, rx: 16385, period: 100, timeout: 120, retries: 1, role: 0, targets: '', autoStart: false });
    radioTrace.value = [];
    traceSupported.value = null;
    traceFetchedAt.value = '';
    traceError.value = '';
    radioTraceDetail.value = [];
    traceDetailSupported.value = null;
    traceDetailFetchedAt.value = '';
    traceDetailError.value = '';
}
async function handleUsbLoss(message = 'USB connection lost. Reconnect the device to continue.') {
    if (losingConnection || !connected.value) return;
    losingConnection = true;
    connected.value = false;
    active.value = 'overview';
    clearDeviceSession();
    await invoke('disconnect').catch(() => { });
    missingPortChecks = 0;
    selectedPort.value = '';
    error.value = message;
    notice.value = '';
    log('USB connection lost');
    await refreshPorts();
    losingConnection = false;
}
async function endAntennaCalibrationSession() {
    if (!calibration.active) return;
    await pauseLiveCalibrationLoop();
    if (connected.value && calibration.originalRole !== 1) {
        await cmd(0x20, [calibration.originalRole]);
        await refreshStatus();
    }
    calibration.active = false;
    calibration.phase = 'idle';
    calibration.stopRequested = false;
    calibration.error = '';
    calibration.message = calibration.isSaved
        ? 'Live calibration stopped. The saved antenna delays remain active.'
        : 'Live calibration stopped. The selected antenna delays remain active in RAM but are not saved.';
}
async function navigate(tab: string) {
    if (calibration.operationBusy) return;
    if (calibration.active && tab !== 'calibration') {
        try { await endAntennaCalibrationSession(); }
        catch (e: any) { error.value = `Could not stop live calibration cleanly: ${String(e)}`; return; }
    }
    if (tab !== 'overview' && !connected.value) {
        active.value = 'overview';
        return;
    }
    active.value = tab;
    if (tab === 'bluetooth' && ble.supported) void refreshBluetooth();
}
async function refreshPorts() {
    try {
        const available = await invoke<Port[]>('list_ports');
        ports.value = available;
        if (connected.value && selectedPort.value) {
            const present = available.some(p => p.name === selectedPort.value);
            missingPortChecks = present ? 0 : missingPortChecks + 1;
            if (missingPortChecks >= 2) {
                await handleUsbLoss('The USB device is no longer available. Reconnect it to continue.');
            }
        } else if (!connected.value && (!selectedPort.value || !available.some(p => p.name === selectedPort.value))) {
            selectedPort.value = available[0]?.name ?? '';
        }
    } catch {
        // Port enumeration can fail transiently; the active status poll is a second signal.
    }
}
async function connectDevice() {
    if (!selectedPort.value || connecting.value) return;
    connecting.value = true;
    error.value = '';
    notice.value = '';
    try {
        await invoke('connect', { portName: selectedPort.value });
        connected.value = true;
        missingPortChecks = 0;
        radioTrace.value = [];
        traceSupported.value = null;
        traceFetchedAt.value = '';
        traceError.value = '';
        radioTraceDetail.value = [];
        traceDetailSupported.value = null;
        traceDetailFetchedAt.value = '';
        traceDetailError.value = '';
        log(`USB connected · ${selectedPort.value}`);
        await cmd(0x01);
        await refreshAll();
        await cmd(0x32, u16(0x7fff));
        notice.value = 'Device connected';
    } catch (e: any) {
        error.value = String(e);
        await invoke('disconnect').catch(() => { });
        connected.value = false;
        active.value = 'overview';
        clearDeviceSession();
    } finally {
        connecting.value = false;
    }
}
async function disconnectDevice() {
    if (calibration.operationBusy) return;
    if (calibration.active) {
        try { await endAntennaCalibrationSession(); } catch { /* Disconnect still needs to complete. */ }
    }
    await invoke('disconnect').catch(() => { });
    connected.value = false;
    active.value = 'overview';
    clearDeviceSession();
    missingPortChecks = 0;
    notice.value = '';
    log('USB disconnected');
    await refreshPorts();
}
function syncDraft() {
    // The Configuration page is populated only from the NVS snapshot returned
    // by GET_SAVED_CONFIG, never from the runtime role/targets/schedule.
    Object.assign(draft, {
        deviceId: device.config.deviceId,
        panId: device.config.panId.toString(16).toUpperCase().padStart(4, '0'),
        channel: device.config.channel,
        tx: device.config.tx,
        rx: device.config.rx,
        period: device.config.period,
        timeout: device.config.timeout,
        retries: device.config.retries,
        role: device.config.role,
        targets: device.savedTargets.join(', '),
        autoStart: device.config.autoStart
    });
}
async function refreshStatus() {
    const p = await cmd(0x03);
    device.status = {
        role: p[0], ranging: p[1] === 1, radio_ready: p[2] === 1, standby: p[3] === 1,
        target_count: p[4], clock: p[5] === 1, uptime: get32(p, 6), ok: get32(p, 10), failed: get32(p, 14)
    };
    applyBleStatusPayload(p);
}
async function refreshAll() {
    busy.value = true;
    try {
        // GET_STATUS is live runtime state. GET_CONFIG is live RAM settings.
        // GET_SAVED_CONFIG is the independent NVS snapshot shown in Configuration.
        const [i, s, runtime, savedPayload, st, t, sc, d, le] = await Promise.all([
            cmd(0x02), cmd(0x03), cmd(0x10), cmd(0x17), cmd(0x04), cmd(0x22), cmd(0x24), cmd(0x35), cmd(0x06)
        ]);
        const saved = decodeSavedConfig(savedPayload);
        applyBleStatusPayload(s);
        const runtimeBleConfig = runtime.length >= 32 ? decodeBleConfigExtension(runtime, 16) : defaultBleConfig();
        ble.supported = ble.supported && runtime.length >= 32 && saved.bleConfigAvailable;
        Object.assign(ble.runtimeConfig, cloneBleConfig(runtimeBleConfig));
        Object.assign(ble.savedConfig, cloneBleConfig(saved.bleConfig));
        Object.assign(bleDraft, cloneBleConfig(saved.bleConfig));
        blePeersInput.value = bleDraft.peers.join(', ');
        ble.peerStatuses = [];
        ble.discoveredPeers = [];
        device.info = {
            version: `${i[0]}.${i[1]}.${i[2]}`, protocol: i[3], id: get16(i, 4),
            maxTargets: i[6], history: get16(i, 7), radio: i[9], maxPayload: get16(i, 10)
        };
        device.status = {
            role: s[0], ranging: s[1] === 1, radio_ready: s[2] === 1, standby: s[3] === 1,
            target_count: s[4], clock: s[5] === 1, uptime: get32(s, 6), ok: get32(s, 10), failed: get32(s, 14)
        };
        device.runtimeConfig = {
            deviceId: get16(runtime), panId: get16(runtime, 2), channel: runtime[4],
            tx: get16(runtime, 5), rx: get16(runtime, 7), period: get32(runtime, 9),
            timeout: get16(runtime, 13), retries: runtime[15]
        };
        device.config = {
            deviceId: saved.deviceId, panId: saved.panId, channel: saved.channel,
            tx: saved.tx, rx: saved.rx, period: saved.period, timeout: saved.timeout,
            retries: saved.retries, role: saved.role, autoStart: saved.autoStart
        };
        device.savedTargets = [...saved.targets];
        device.stats = {
            ok: get32(st), failed: get32(st, 4), rx: get32(st, 8), tx: get32(st, 12),
            timeouts: get32(st, 16), protocol: get32(st, 20), overflows: get32(st, 24),
            overwrites: get32(st, 28), malformed: get32(st, 32), duplicates: get32(st, 36)
        };
        // These commands intentionally describe current RAM state for the Ranging test page.
        device.targets = Array.from({ length: t[0] }, (_, j) => get16(t, 1 + j * 2));
        device.schedule = {
            period: get32(sc), timeout: get16(sc, 4), retries: sc[6], targetCount: sc[7],
            queued: sc[8], active: sc[9] === 1, effective: get32(sc, 10), average: get32(sc, 14)
        };
        device.diagnostics = {
            radio: get32(d), live: get32(d, 4), malformed: get32(d, 8), rx: get32(d, 12),
            tx: get32(d, 16), state: d[20], standby: d[21] === 1
        };
        device.lastError = { code: le[0], time: get64(le, 1).toString() };
        syncDraft();
        lastRefresh.value = new Date().toLocaleTimeString();
    } finally {
        busy.value = false;
    }
}
async function saveSettings() {
    if (savingConfig.value) return;
    error.value = '';
    try {
        const targets = draft.targets.trim() ? draft.targets.split(',').map(x => Number(x.trim())) : [];
        if (targets.length > 16 || targets.some(x => !Number.isInteger(x) || x < 1 || x > 65534) ||
            targets.includes(draft.deviceId) || new Set(targets).size !== targets.length) {
            throw new Error('Targets must be unique IDs from 1 to 65534 (maximum 16).');
        }
        const pan = parseInt(draft.panId, 16);
        if (!Number.isInteger(pan) || pan < 0 || pan > 65535) throw new Error('PAN ID must be a 4-digit hexadecimal value.');
        if (!Number.isInteger(draft.deviceId) || draft.deviceId < 1 || draft.deviceId > 65534) throw new Error('Device ID must be 1–65534.');
        if (draft.channel !== 5 && draft.channel !== 9) throw new Error('UWB channel must be 5 or 9.');
        if (!Number.isInteger(draft.tx) || draft.tx < 0 || draft.tx > 65535 ||
            !Number.isInteger(draft.rx) || draft.rx < 0 || draft.rx > 65535) throw new Error('Antenna delays must be uint16 values (0–65535).');
        if (draft.period < 20 || draft.period > 60000 || draft.timeout < 50 || draft.timeout > 5000 ||
            draft.retries < 0 || draft.retries > 5 || !Number.isInteger(draft.role) || draft.role < 0 || draft.role > 2) {
            throw new Error('Check the schedule limits and operating role.');
        }
        if (draft.autoStart && (draft.role === 0 || (draft.role === 1 && targets.length === 0))) {
            throw new Error('Auto-start requires an Initiator with at least one target, or a Responder role.');
        }
        if (device.status?.ranging) throw new Error('Stop the RAM ranging test before saving configuration.');
        if (ble.supported) {
            bleDraft.peers = parseBlePeerIds(blePeersInput.value);
            validateBleConfig(bleDraft);

        }
        savingConfig.value = true;

        // One explicit snapshot, including the BLE extension, is applied to runtime RAM and NVS.
        // It cannot accidentally persist temporary settings changed by Ranging test.
        const payload = [
            ...u16(draft.deviceId), ...u16(pan), draft.channel, ...u16(draft.tx), ...u16(draft.rx),
            ...u32(draft.period), ...u16(draft.timeout), draft.retries, draft.role,
            targets.length, ...targets.flatMap(u16), draft.autoStart ? 1 : 0,
            ...(ble.supported ? encodeBleConfigExtension(bleDraft) : [])
        ];
        savingStep.value = 'Applying configuration and committing this exact snapshot to NVS…';
        try {
            await cmd(0x18, payload);
        } catch (applyError: any) {
            // If the ACK was lost after commit, require both persistent and runtime
            // readback to match. NVS alone is insufficient if the command was BUSY.
            const [savedPayload, runtime, status, runtimeTargetsPayload] = await Promise.all([
                cmd(0x17), cmd(0x10), cmd(0x03), cmd(0x22)
            ]);
            const readback = decodeSavedConfig(savedPayload);
            const runtimeTargets = Array.from({ length: runtimeTargetsPayload[0] }, (_, i) => get16(runtimeTargetsPayload, 1 + i * 2));
            const savedMatches = readback.deviceId === draft.deviceId && readback.panId === pan &&
                readback.channel === draft.channel && readback.tx === draft.tx && readback.rx === draft.rx &&
                readback.period === draft.period && readback.timeout === draft.timeout &&
                readback.retries === draft.retries && readback.role === draft.role &&
                readback.autoStart === draft.autoStart && JSON.stringify(readback.targets) === JSON.stringify(targets) &&
                (!ble.supported || (readback.bleConfigAvailable && JSON.stringify(readback.bleConfig) === JSON.stringify(bleDraft)));
            const runtimeBleMatches = !ble.supported || (runtime.length >= 32 &&
                JSON.stringify(decodeBleConfigExtension(runtime, 16)) === JSON.stringify(bleDraft));
            const runtimeMatches = get16(runtime, 0) === draft.deviceId && get16(runtime, 2) === pan &&
                runtime[4] === draft.channel && get16(runtime, 5) === draft.tx && get16(runtime, 7) === draft.rx &&
                get32(runtime, 9) === draft.period && get16(runtime, 13) === draft.timeout &&
                runtime[15] === draft.retries && status[0] === draft.role &&
                JSON.stringify(runtimeTargets) === JSON.stringify(targets) && runtimeBleMatches;
            if (!savedMatches || !runtimeMatches) throw applyError;
            log('APPLY_SAVE_CONFIG acknowledgement lost; NVS and runtime readback match the submitted configuration');
        }

        await refreshAll();
        notice.value = device.status?.radio_ready
            ? 'Configuration applied to RAM and saved to NVS. The firmware manages any required BLE restart and reconnection automatically.'
            : 'Configuration was saved to NVS and loaded into RAM, but the radio is not ready. Check diagnostics.';
        log(`Configuration saved · UWB ${['Idle', 'Initiator', 'Responder'][draft.role]} · BLE ${bleModeName(ble.savedConfig.mode)}`);
    } catch (e: any) {
        error.value = `${String(e)} The saved NVS configuration may be unchanged or partially committed; use Reload to read NVS.`;
    } finally {
        savingConfig.value = false;
        savingStep.value = '';
    }
}

async function pumpCalibrationEvents(jobId: number) {
    // Firmware sends range results as USB events. Short status reads keep the
    // serial stream draining while a one-shot ranging transaction is active.
    while (calibrationWaiters.has(jobId) && connected.value) {
        try {
            await cmd(0x03);
        } catch (e: any) {
            const waiter = calibrationWaiters.get(jobId);
            if (waiter) {
                window.clearTimeout(waiter.timer);
                calibrationWaiters.delete(jobId);
                waiter.reject(e instanceof Error ? e : new Error(String(e)));
            }
            return;
        }
        if (calibrationWaiters.has(jobId)) await new Promise<void>(resolve => window.setTimeout(resolve, 15));
    }
}
function waitForCalibrationJob(jobId: number, timeoutMs = 7000): Promise<CalJobResult> {
    const cached = calibrationJobResults.get(jobId);
    if (cached) {
        calibrationJobResults.delete(jobId);
        return Promise.resolve(cached);
    }
    return new Promise((resolve, reject) => {
        const timer = window.setTimeout(() => {
            calibrationWaiters.delete(jobId);
            reject(new Error(`No range result arrived for job ${jobId} within ${timeoutMs} ms.`));
        }, timeoutMs);
        calibrationWaiters.set(jobId, { resolve, reject, timer });
        void pumpCalibrationEvents(jobId);
    });
}
async function applyCalibrationDelay(tx: number, rx = tx) {
    if (![tx, rx].every(value => Number.isInteger(value) && value >= 0 && value <= 65535)) {
        throw new Error('Antenna delays must be uint16 values (0–65535).');
    }
    await cmd(0x16, [...u16(tx), ...u16(rx)]);
    const readback = await cmd(0x15);
    if (get16(readback, 0) !== tx || get16(readback, 2) !== rx) {
        throw new Error('Runtime antenna-delay readback does not match the requested values.');
    }
    calibration.appliedTx = tx;
    calibration.appliedRx = rx;
    calibration.delayPending = false;
    calibration.isSaved = tx === calibration.savedTx && rx === calibration.savedRx;
    if (device.runtimeConfig) {
        device.runtimeConfig.tx = tx;
        device.runtimeConfig.rx = rx;
    }
}
function clearCalibrationAverage() {
    calibration.windowValues = [];
    calibration.averageMm = null;
    calibration.latestMm = null;
    calibration.validSamples = 0;
    calibration.failedSamples = 0;
}
function addCalibrationMeasurement(mm: number) {
    calibration.latestMm = mm;
    calibration.windowValues.push(mm);
    if (calibration.windowValues.length > CAL_WINDOW_SIZE) calibration.windowValues.shift();
    calibration.validSamples++;
    calibration.averageMm = calibration.windowValues.reduce((sum, value) => sum + value, 0) / calibration.windowValues.length;
}
async function runLiveCalibrationLoop() {
    calibration.processing = true;
    let consecutiveFailures = 0;
    const registerFailure = (reason: string) => {
        calibration.failedSamples++;
        consecutiveFailures++;
        if (consecutiveFailures >= 5) {
            calibration.connectionUnstable = true;
            calibration.connectionWarning = `Connection unstable: ${consecutiveFailures} consecutive range attempts failed (${reason}).`;
        }
    };
    const retryPause = () => new Promise<void>(resolve => window.setTimeout(resolve, 250));

    try {
        while (calibration.active && !calibration.stopRequested && connected.value) {
            // Apply a changed slider value only between completed ranging transactions.
            const requested = calibration.requestedDelay;
            if (calibration.delayPending) await applyCalibrationDelay(requested, requested);
            if (calibration.stopRequested) break;

            let response: number[];
            try {
                response = await cmd(0x27, u16(Number(calibration.referenceId)));
            } catch (e: any) {
                if (calibration.stopRequested || !connected.value) break;
                registerFailure(String(e));
                await retryPause();
                continue;
            }

            const jobId = get16(response, 0);
            if (!jobId) {
                registerFailure('the device returned an invalid one-shot job ID');
                await retryPause();
                continue;
            }
            calibration.activeJobId = jobId;
            let result: CalJobResult;
            try {
                result = await waitForCalibrationJob(jobId);
            } catch (e: any) {
                calibration.activeJobId = 0;
                try { await cmd(0x28, u16(jobId)); } catch { /* It may already have completed. */ }
                if (calibration.stopRequested || !connected.value) break;
                registerFailure(String(e));
                await retryPause();
                continue;
            }
            calibration.activeJobId = 0;
            if (result.target === Number(calibration.referenceId) && result.status === 0 && result.mm !== null && result.mm >= 0) {
                addCalibrationMeasurement(result.mm);
                consecutiveFailures = 0;
                calibration.connectionUnstable = false;
                calibration.connectionWarning = '';
            } else {
                registerFailure(statusText(result.status));
                await retryPause();
            }
        }
    } catch (e: any) {
        if (!calibration.stopRequested && connected.value) {
            calibration.connectionUnstable = false;
            calibration.connectionWarning = '';
            calibration.error = String(e);
            calibration.phase = 'error';
            log(`Live antenna calibration stopped · ${String(e)}`);
        }
    } finally {
        calibration.processing = false;
        calibration.activeJobId = 0;
    }
}
function startLiveCalibrationLoop() {
    if (!calibration.active || calibration.processing || !connected.value) return;
    calibration.stopRequested = false;
    calibrationLoopPromise = runLiveCalibrationLoop();
}
async function pauseLiveCalibrationLoop() {
    calibration.stopRequested = true;
    if (calibration.activeJobId) {
        // Let the current one-shot finish naturally. If it times out, the loop
        // cancels that job and then resolves this pause operation.
    }
    if (calibrationLoopPromise) await calibrationLoopPromise;
    calibrationLoopPromise = null;
}
async function beginAntennaCalibration() {
    if (calibration.active || !connected.value) return;
    calibration.error = '';
    calibration.connectionUnstable = false;
    calibration.connectionWarning = '';
    calibration.message = '';
    calibration.phase = 'starting';
    const referenceId = Number(calibration.referenceId);
    if (!Number.isInteger(referenceId) || referenceId < 1 || referenceId > 65534) {
        calibration.error = 'Enter a valid responder ID from 1 to 65534.'; calibration.phase = 'error'; return;
    }
    if (referenceId === device.info?.id) {
        calibration.error = 'The responder ID must differ from the connected module ID.'; calibration.phase = 'error'; return;
    }
    if (device.status?.ranging) {
        calibration.error = 'Stop continuous ranging before starting antenna calibration.'; calibration.phase = 'error'; return;
    }
    try {
        const status = await cmd(0x03);
        if (status[1] === 1) throw new Error('Stop continuous ranging before starting antenna calibration.');
        if (status[2] !== 1 || status[3] === 1) throw new Error('The connected radio must be ready and awake.');
        const schedule = await cmd(0x24);
        if (schedule[8] !== 0 || schedule[9] !== 0) throw new Error('Wait for active and queued range transactions to finish.');
        const subscription = await cmd(0x32, u16(0x0003));
        if (subscription.length < 2 || (get16(subscription, 0) & 0x0003) !== 0x0003) {
            throw new Error('Could not subscribe to range result and failure events on this USB connection.');
        }
        const [antenna, savedPayload] = await Promise.all([cmd(0x15), cmd(0x17)]);
        calibration.originalRole = status[0];
        const saved = decodeSavedConfig(savedPayload);
        calibration.savedTx = saved.tx;
        calibration.savedRx = saved.rx;
        calibration.appliedTx = get16(antenna, 0);
        calibration.appliedRx = get16(antenna, 2);
        calibration.requestedDelay = Math.round((calibration.appliedTx + calibration.appliedRx) / 2);
        calibration.delayPending = false;
        calibration.isSaved = calibration.appliedTx === saved.tx && calibration.appliedRx === saved.rx;
        clearCalibrationAverage();
        if (status[0] !== 1) { await cmd(0x20, [1]); await refreshStatus(); }
        calibration.active = true;
        calibration.phase = 'live';
        calibration.error = '';
        calibration.message = 'Live ranging is running. Adjust the delay until the averaged reading matches the physical antenna separation.';
        log(`Live antenna calibration started · responder ${referenceId} · current TX/RX ${calibration.appliedTx}/${calibration.appliedRx}`);
        startLiveCalibrationLoop();
    } catch (e: any) {
        calibration.active = false;
        calibration.phase = 'error';
        calibration.error = String(e);
    }
}
function setCalibrationDelay(event: Event) {
    const raw = (event.target as HTMLInputElement).value;
    if (raw.trim() === '') return;
    const value = Number(raw);
    if (!Number.isInteger(value) || value < 0 || value > 65535) return;
    calibrationDelayInput.value = String(value);
    if (value !== calibration.requestedDelay) calibration.delayPending = true;
    calibration.requestedDelay = value;
    calibration.isSaved = !calibration.delayPending && value === calibration.savedTx && value === calibration.savedRx;
    calibration.message = '';
}

function commitCalibrationDelayInput() {
    const raw = String(calibrationDelayInput.value).trim();
    if (raw === '') {
        calibrationDelayInput.value = String(calibration.requestedDelay);
        return;
    }
    const value = Number(raw);
    if (!Number.isInteger(value) || value < 0 || value > 65535) {
        calibrationDelayInput.value = String(calibration.requestedDelay);
        return;
    }
    if (value !== calibration.requestedDelay) calibration.delayPending = true;
    calibration.requestedDelay = value;
    calibrationDelayInput.value = String(value);
    calibration.isSaved = !calibration.delayPending && value === calibration.savedTx && value === calibration.savedRx;
    calibration.message = '';
}
async function saveAntennaCalibration() {
    if (!calibration.active || calibration.operationBusy || !connected.value) return;
    calibration.operationBusy = true;
    calibration.phase = 'saving';
    calibration.error = '';
    try {
        await pauseLiveCalibrationLoop();
        // Commit the latest slider position after the in-flight measurement has ended.
        if (calibration.delayPending || calibration.requestedDelay !== calibration.appliedTx || calibration.requestedDelay !== calibration.appliedRx) {
            await applyCalibrationDelay(calibration.requestedDelay, calibration.requestedDelay);
        }
        const savedPair = await cmd(0x19);
        if (savedPair.length !== 4 || get16(savedPair, 0) !== calibration.requestedDelay || get16(savedPair, 2) !== calibration.requestedDelay) {
            throw new Error('The firmware did not confirm the requested antenna delays were saved to NVS.');
        }
        const verify = decodeSavedConfig(await cmd(0x17));
        if (verify.tx !== calibration.requestedDelay || verify.rx !== calibration.requestedDelay) {
            throw new Error('NVS readback does not match the selected TX/RX antenna delay.');
        }
        calibration.savedTx = verify.tx;
        calibration.savedRx = verify.rx;
        calibration.isSaved = true;
        device.config.tx = verify.tx;
        device.config.rx = verify.rx;
        draft.tx = verify.tx;
        draft.rx = verify.rx;
        calibration.message = `TX and RX antenna delays saved to NVS at ${verify.tx} ticks.`;
        notice.value = `Antenna delays saved · TX=RX=${verify.tx}`;
        log(`Antenna delays saved to NVS · TX=RX=${verify.tx} ticks`);
    } catch (e: any) {
        calibration.error = String(e);
        calibration.phase = 'error';
    } finally {
        calibration.operationBusy = false;
        if (calibration.active && connected.value) {
            if (!calibration.error) calibration.phase = 'live';
            startLiveCalibrationLoop();
        }
    }
}
async function resetAntennaCalibration() {
    if (!calibration.active || calibration.operationBusy || !connected.value) return;
    calibration.operationBusy = true;
    calibration.phase = 'resetting';
    calibration.error = '';
    try {
        await pauseLiveCalibrationLoop();
        const saved = decodeSavedConfig(await cmd(0x17));
        await applyCalibrationDelay(saved.tx, saved.rx);
        calibration.savedTx = saved.tx;
        calibration.savedRx = saved.rx;
        calibration.requestedDelay = Math.round((saved.tx + saved.rx) / 2);
        calibration.delayPending = false;
        calibration.isSaved = true;
        clearCalibrationAverage();
        device.config.tx = saved.tx;
        device.config.rx = saved.rx;
        draft.tx = saved.tx;
        draft.rx = saved.rx;
        calibration.message = `Reloaded saved NVS antenna delays: TX ${saved.tx} ticks · RX ${saved.rx} ticks.`;
        log(`Antenna delays restored from NVS · TX=${saved.tx} RX=${saved.rx}`);
    } catch (e: any) {
        calibration.error = String(e);
        calibration.phase = 'error';
    } finally {
        calibration.operationBusy = false;
        if (calibration.active && connected.value) {
            if (!calibration.error) calibration.phase = 'live';
            startLiveCalibrationLoop();
        }
    }
}

async function resetConfig() {
    if (!confirm('Reset saved UWB and Bluetooth configuration, peer lists, targets and roles to compiled defaults? Bluetooth will stop and the UWB radio will reinitialize.')) return;
    try {
        await cmd(0x13);
        await refreshAll();
        notice.value = 'Device restored to compiled defaults';
        log('Configuration reset');
    } catch (e: any) {
        error.value = String(e);
    }
}
async function setRole(role: number) {
    try {
        // This is a runtime-only test control. Never issue SAVE_CONFIG here.
        await cmd(0x20, [role]);
        await refreshStatus();
        notice.value = `Runtime role changed to ${['Idle', 'Initiator', 'Responder'][role]}. NVS configuration was not changed.`;
        log(`Temporary RAM role set · ${roleName.value}`);
    } catch (e: any) {
        error.value = String(e);
    }
}
async function startRange() { try { await cmd(0x25); await refreshStatus(); notice.value = 'Ranging started'; } catch (e: any) { error.value = String(e) } }
async function stopRange() { try { await cmd(0x26); await refreshStatus(); notice.value = 'Ranging stopped and queued one-shots cleared'; } catch (e: any) { error.value = String(e) } }
async function rangeOnce() { try { const target = Number(oneShotTarget.value); if (!Number.isInteger(target) || target < 1 || target > 65534) throw new Error('Enter a valid target ID.'); const p = await cmd(0x27, u16(target)); notice.value = `One-shot job ${get16(p)} queued for target ${target}`; log(`RANGE_ONCE queued · target ${target} · job ${get16(p)}`) } catch (e: any) { error.value = String(e) } }
const oneShotTarget = ref('2'), cancelJobId = ref('1'), advancedOutput = ref('No advanced command run yet.');
async function loadHistory() { try { let cursor = 0, rows: any[] = []; for (let page = 0; page < 16; page++) { const p = await cmd(0x31, [...u32(cursor), ...u16(17), ...u16(0xffff)]); const count = p[13]; cursor = get32(p, 8); for (let j = 0; j < count; j++)rows.push(decodeMeasurement(p.slice(14 + j * 28, 14 + (j + 1) * 28))); if (count < 17 || cursor === 0) break; } measurements.value = rows.reverse().slice(-256); notice.value = `Loaded ${measurements.value.length} retained measurements`; } catch (e: any) { error.value = String(e) } }
async function quick(op: number, label: string) { try { const p = await cmd(op); notice.value = label; log(label); return p } catch (e: any) { error.value = String(e); return null } }
async function pingNode() { const p = await quick(1, 'PING completed'); if (p) advancedOutput.value = `PING response: ${readString(p)}` }
async function syncClock() { try { const us = BigInt(Date.now()) * 1000n; const p = await cmd(0x14, Array.from({ length: 8 }, (_, i) => Number((us >> BigInt(8 * i)) & 255n))); advancedOutput.value = `TIME_SYNC · ESP monotonic: ${get64(p).toString()} µs · epoch offset: ${BigInt.asIntN(64, get64(p, 8)).toString()} µs`; notice.value = 'Device clock synchronized'; } catch (e: any) { error.value = String(e) } }
async function readLast() { try { const p = await cmd(0x30, [0xff, 0xff]); const count = p[0]; const rows = Array.from({ length: count }, (_, i) => decodeMeasurement(p.slice(1 + i * 28, 1 + (i + 1) * 28))); measurements.value = rows; advancedOutput.value = `GET_LAST returned ${count} newest retained measurement(s), one per target.`; navigate('history'); } catch (e: any) { error.value = String(e) } }
async function cancelJob() { try { await cmd(0x28, u16(Number(cancelJobId.value))); advancedOutput.value = `Cancel request sent for job ${cancelJobId.value}.`; notice.value = 'Operation cancellation acknowledged'; } catch (e: any) { error.value = String(e) } }
async function subscribeAll() { try { const p = await cmd(0x32, u16(0x7fff)); advancedOutput.value = `Active event subscription mask: ${hex(get16(p))}`; notice.value = 'Subscribed to all UWB and BLE event types'; } catch (e: any) { error.value = String(e) } }
async function unsubscribeAll() { try { const p = await cmd(0x33, u16(0x7fff)); advancedOutput.value = `Active event subscription mask: ${hex(get16(p))}`; notice.value = 'Unsubscribed from all UWB and BLE event types'; } catch (e: any) { error.value = String(e) } }
async function readAntenna() { try { const p = await cmd(0x15); advancedOutput.value = `TX antenna delay: ${get16(p)} · RX antenna delay: ${get16(p, 2)}`; } catch (e: any) { error.value = String(e) } }
async function clearStats() { if (confirm('Clear all device diagnostic counters?')) { await quick(5, 'Statistics counters cleared'); await refreshAll() } }
async function clearHistory() { if (confirm('Clear retained measurement history?')) { await quick(0x34, 'Measurement history cleared'); measurements.value = [] } }
async function sleepRadio() { await quick(0x40, 'Radio entered IDLE_RC standby (not deep sleep)'); await refreshStatus() }
async function wakeRadio() { await quick(0x41, 'Radio wake command completed'); await refreshStatus() }
async function reboot() { if (confirm('Reboot the ESP32 node? The USB connection will be interrupted.')) { await quick(0x43, 'Reboot command sent'); setTimeout(() => { void handleUsbLoss('Device rebooted. Reconnect when it appears on USB.'); }, 1000); } }
async function systemSleep() { const sec = Number(prompt('Timer wake duration in seconds (1–604800):', '60')); if (!Number.isInteger(sec) || sec < 1 || sec > 604800) return; try { await cmd(0x42, u32(sec * 1000)); await handleUsbLoss('Device entered deep sleep. Reconnect after timer wake.'); } catch (e: any) { error.value = String(e) } }
function fmtDuration(ms: number) { const s = Math.floor(ms / 1000); return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m ${s % 60}s` }
function statusText(s: number) { return ['Valid', 'Timeout', 'Invalid', 'Cancelled', 'Radio error'][s] ?? 'Unknown' }
function fmtRange(mm: number | null) { return mm === null || mm < 0 ? '—' : `${(mm / 1000).toFixed(3)} m` }
watch(connected, (isConnected) => {
    if (!isConnected) active.value = 'overview';
});
onMounted(async () => {
    await refreshPorts();
    unlisten = await listen<FrameEvent>('uwb-event', e => handleEvent(e.payload));
    poller = window.setInterval(async () => {
        await refreshPorts();
        if (connected.value && !polling && !savingConfig.value && !calibration.active && !calibration.processing && !calibration.activeJobId) {
            polling = true;
            try {
                await refreshStatus();
                if (active.value === 'bluetooth' && ble.supported && Date.now() - lastBleRefreshAt >= 4000) {
                    lastBleRefreshAt = Date.now();
                    await refreshBluetooth();
                }
            } catch (e: any) {
                if (isUsbDisconnectError(e)) {
                    await handleUsbLoss('USB connection lost. Reconnect the device to continue.');
                }
            } finally {
                polling = false;
            }
        }
    }, 1200);
});
onUnmounted(() => {
    if (poller) clearInterval(poller);
    unlisten?.();
});
</script>

<template>
    <div class="shell">
        <aside class="sidebar">
            <div class="brand">
                <div class="brand-mark">
                    <Antenna :size="22" />
                </div>
                <div>
                    <div class="brand-name">D3KM</div>
                    <div class="brand-sub">DWM3000 MANAGER</div>
                </div>
            </div>
            <div class="device-mini">
                <div class="device-glyph">
                    <Cpu :size="19" />
                </div>
                <div class="device-copy"><strong>{{ connected ? (device.info ? `Node ${device.info.id}` : 'USB device')
                    : 'No device' }}</strong><span>{{ connected ? selectedPort : 'Connect a node to begin' }}</span>
                </div><span class="dot" :class="{ online: connected && device.status?.radio_ready }" />
            </div>
            <div class="nav-label">WORKSPACE</div>
            <nav>
                <button :class="{ selected: active === 'overview' }" @click="navigate('overview')">
                    <LayoutDashboard />Overview
                </button>
                <button :class="{ selected: active === 'configuration' }" :disabled="!connected" @click="navigate('configuration')">
                    <Settings2 />Configuration<span v-if="dirty" class="dirty-dot" />
                </button>
                <button :class="{ selected: active === 'calibration' }" :disabled="!connected" @click="navigate('calibration')">
                    <Antenna />Antenna calibration
                </button>
                <button :class="{ selected: active === 'ranging' }" :disabled="!connected" @click="navigate('ranging')">
                    <Waves />Ranging test
                </button>
                <button :class="{ selected: active === 'bluetooth' }" :disabled="!connected || !ble.supported" @click="navigate('bluetooth')">
                    <Bluetooth />Bluetooth<span v-if="ble.runtime.connected" class="nav-live-dot" />
                </button>
                <button :class="{ selected: active === 'history' }" :disabled="!connected" @click="navigate('history')">
                    <History />Measurements
                </button>
                <button :class="{ selected: active === 'diagnostics' }" :disabled="!connected" @click="navigate('diagnostics')">
                    <Activity />Diagnostics
                </button>
                <button :class="{ selected: active === 'system' }" :disabled="!connected" @click="navigate('system')">
                    <Power />Device controls
                </button>
            </nav>
            <div class="sidebar-bottom">
                <div class="protocol-label">LINK PROTOCOL</div>
                <div class="protocol-value"><span class="pulse" />UART protocol v1</div>
                <div class="protocol-meta">COBS · CRC-16/CCITT-FALSE<br />USB CDC · 460800 line coding</div>
                <div class="sidebar-version">D3KM <span>v0.2.0</span></div>
            </div>
        </aside>
        <main class="main">
            <header class="topbar">
                <div class="breadcrumb">D3KM <span>/</span>
                    {{ ({
                        overview: 'Overview', configuration: 'Configuration', calibration: 'Antenna calibration', ranging: 'Ranging test', bluetooth: 'Bluetooth messaging', history:'Measurements',diagnostics:'Diagnostics',system:'Device controls'} as any)[active]}}
                </div>
                <div class="top-actions">
                    <div class="connection-pill" :class="{ connected }"><span class="dot"
                            :class="{ online: connected }" />{{ connected ? 'USB connected' : 'Not connected' }}</div>
                    <button class="icon-button" title="Refresh ports" @click="refreshPorts">
                        <RefreshCw :size="16" />
                    </button><button v-if="connected" class="button secondary small" :disabled="calibration.operationBusy" @click="disconnectDevice">
                        <Unplug :size="15" /> Disconnect
                    </button>
                </div>
            </header>
            <div class="content">
                <div v-if="error" class="alert error">
                    <CircleAlert :size="17" /><span>{{ error }}</span><button @click="error = ''">×</button>
                </div>
                <div v-if="notice" class="alert success">
                    <BadgeCheck :size="17" /><span>{{ notice }}</span><button @click="notice = ''">×</button>
                </div>
                <section v-if="active === 'overview'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">DEVICE MANAGEMENT</div>
                            <h1>Device overview</h1>
                            <p>Inspect UWB ranging and Bluetooth messaging readiness across the connected node.</p>
                        </div><button class="button primary" :disabled="!connected || busy" @click="refreshAll">
                            <RefreshCw :size="16" /> Refresh device
                        </button>
                    </div>
                    <div v-if="!connected" class="connect-card">
                        <div class="connect-icon">
                            <Cable :size="24" />
                        </div>
                        <div class="connect-text">
                            <h2>Connect a DWM3000 node</h2>
                            <p>Connect the ESP32-C3 device over USB. D3KM communicates using the binary USB CDC
                                protocol.</p>
                            <div class="connect-row"><select v-model="selectedPort">
                                    <option value="" disabled>Select USB serial device</option>
                                    <option v-for="p in ports" :key="p.name" :value="p.name">{{ p.name }} —
                                        {{ p.description }}</option>
                                </select><button class="button primary" :disabled="!selectedPort || connecting"
                                    @click="connectDevice">
                                    <Plug :size="16" /> {{ connecting ? 'Connecting…' : 'Connect device' }}
                                </button><button class="button secondary" @click="refreshPorts">
                                    <RefreshCw :size="15" />
                                </button></div>
                            <div class="hint">USB CDC is a native USB data link. The 460800 value is line coding, not a
                                USB transfer clock.</div>
                        </div>
                    </div>
                    <template v-else>
                        <div class="metric-grid">
                            <div class="metric-card">
                                <div class="metric-top"><span>DEVICE HEALTH</span>
                                    <ShieldCheck />
                                </div>
                                <div class="metric-value" :class="device.status?.radio_ready ? 'good' : 'bad'">{{ health
                                    }}
                                </div>
                                <div class="metric-foot">{{ device.status?.radio_ready ? 'DWM3000 radio initialized' :
                                    'Check radio diagnostics'}}</div>
                            </div>
                            <div class="metric-card">
                                <div class="metric-top"><span>ACTIVE ROLE</span>
                                    <Antenna />
                                </div>
                                <div class="metric-value">{{ roleName }}</div>
                                <div class="metric-foot">{{ device.status?.ranging ? 'Ranging enabled' : 'Ranging stopped' }}
                                </div>
                            </div>
                            <div class="metric-card">
                                <div class="metric-top"><span>SUCCESSFUL RANGES</span>
                                    <ChartNoAxesCombined />
                                </div>
                                <div class="metric-value">{{ (device.status?.ok ?? 0).toLocaleString() }}</div>
                                <div class="metric-foot">{{ device.status?.failed ?? 0 }} failed measurements</div>
                            </div>
                            <div class="metric-card">
                                <div class="metric-top"><span>UPTIME</span>
                                    <Clock3 />
                                </div>
                                <div class="metric-value">{{ device.status ? fmtDuration(device.status.uptime) : '—' }}
                                </div>
                                <div class="metric-foot">Since last boot</div>
                            </div>
                        </div>
                        <div class="two-col">
                            <div class="panel">
                                <div class="panel-heading">
                                    <div>
                                        <h3>Device identity</h3>
                                        <p>Firmware and protocol information</p>
                                    </div><span class="tag neutral">DWM3000</span>
                                </div>
                                <div class="detail-list">
                                    <div><span>Firmware version</span><b>{{ device.info?.version }}</b></div>
                                    <div><span>Protocol version</span><b>v{{ device.info?.protocol }}</b></div>
                                    <div><span>Device ID</span><b>{{ device.info?.id }}
                                            <small>{{ hex(device.info?.id ?? 0) }}</small></b></div>
                                    <div><span>Maximum targets</span><b>{{ device.info?.maxTargets }}</b></div>
                                    <div><span>History capacity</span><b>{{ device.info?.history }} records</b></div>
                                    <div><span>Maximum payload</span><b>{{ device.info?.maxPayload }} bytes</b></div>
                                </div>
                            </div>
                            <div class="panel">
                                <div class="panel-heading">
                                    <div>
                                        <h3>Radio readiness</h3>
                                        <p>Live state reported by the node</p>
                                    </div><span class="tag" :class="device.status?.radio_ready ? 'good' : 'bad'">{{
                                        device.status?.radio_ready ? 'READY' : 'NOT READY'}}</span>
                                </div>
                                <div class="readiness">
                                    <div class="readiness-icon" :class="{ good: device.status?.radio_ready }">
                                        <Antenna :size="26" />
                                    </div>
                                    <div><b>{{ device.status?.standby ? 'IDLE_RC standby' : device.status?.radio_ready ?
                                        'Radio initialized':'Radio unavailable'}}</b>
                                        <p>{{ device.status?.standby ? 'Radio is in IDLE_RC standby; this is not true DWM3000 deep sleep.':device.status?.radio_ready?'Radio is initialized and available for ranging.':'Radio initialization failed or is unavailable.'}}
                                        </p>
                                    </div>
                                </div>
                                <div class="detail-list compact">
                                    <div><span>Clock synchronized</span><b>{{ device.status?.clock ? 'Yes' : 'No' }}</b>
                                    </div>
                                    <div><span>Configured UWB targets</span><b>{{ device.status?.target_count ?? 0 }}</b>
                                    </div>
                                    <div><span>Bluetooth mode</span><b>{{ ble.supported ? bleModeName(ble.runtime.mode) : 'Not supported' }}</b></div>
                                    <div><span>Bluetooth links</span><b>{{ !ble.supported ? '—' : bleConnectedCount ? `${bleConnectedCount} active peer link(s)` : ble.runtime.enabled ? (ble.runtime.advertising ? 'Advertising' : ble.runtime.scanning ? 'Discovering peers' : 'Enabled') : 'Disabled' }}</b></div>
                                    <div><span>Last refresh</span><b>{{ lastRefresh }}</b></div>
                                </div>
                            </div>
                        </div>
                        <div class="panel recent-panel">
                            <div class="panel-heading">
                                <div>
                                    <h3>Recent measurements</h3>
                                    <p>Latest results received from the ranging engine</p>
                                </div><button class="text-button" @click="navigate('history')">View history
                                    <ArrowUpFromLine :size="14" />
                                </button>
                            </div>
                            <div v-if="measurements.length" class="table-wrap">
                                <table>
                                    <thead>
                                        <tr>
                                            <th>SEQUENCE</th>
                                            <th>TARGET</th>
                                            <th>DISTANCE</th>
                                            <th>STATUS</th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        <tr v-for="m in measurements.slice(0, 5)" :key="`${m.seq}-${m.time}`">
                                            <td>#{{ m.seq }}</td>
                                            <td>{{ m.target }}</td>
                                            <td>{{ fmtRange(m.mm < 0 ? null : m.mm) }}</td>
                                            <td><span class="tag" :class="m.status === 0 ? 'good' : 'bad'">{{
                                                    statusText(m.status) }}</span>
                                            </td>
                                        </tr>
                                    </tbody>
                                </table>
                            </div>
                            <div v-else class="empty-state">No measurements received in this session.</div>
                        </div>
                    </template>
                </section>

                <section v-else-if="active === 'calibration'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">RADIO CALIBRATION</div>
                            <h1>Antenna delay calibration</h1>
                            <p>Adjust the antenna delay while watching a live, smoothed range measurement.</p>
                        </div>
                        <span class="tag" :class="calibration.connectionUnstable ? 'warning' : calibration.phase === 'live' ? 'good' : calibration.phase === 'error' ? 'bad' : 'neutral'">
                            {{ calibration.connectionUnstable ? 'CONNECTION UNSTABLE' : calibration.phase === 'live' ? 'LIVE FEEDBACK' : calibration.phase === 'starting' ? 'STARTING' : calibration.phase === 'saving' ? 'SAVING' : calibration.phase === 'resetting' ? 'RESETTING' : calibration.phase === 'error' ? 'CHECK CONNECTION' : 'NOT STARTED' }}
                        </span>
                    </div>

                    <div class="panel calibration-explainer">
                        <b>Position the modules at the physical separation you intend to use.</b>
                        <p>No reference distance needs to be entered. The display reports the measured distance; adjust the delay until the live average agrees with the actual distance between the antenna reference points.</p>
                    </div>

                    <div v-if="!calibration.active" class="panel calibration-setup">
                        <div class="panel-heading">
                            <div>
                                <h3>Ranging setup</h3>
                                <p>The connected module initiates repeated one-shot measurements to a separate responder.</p>
                            </div>
                            <Antenna :size="17" />
                        </div>
                        <label class="calibration-peer-field">Active responder ID
                            <input type="number" min="1" max="65534" v-model="calibration.referenceId" :disabled="calibration.phase === 'starting'" />
                            <small>Short address of the other UWB module. Its radio must be active in Responder mode on the same PAN ID and channel. This module ID is {{ device.info?.id ?? '—' }}.</small>
                        </label>
                        <div class="calibration-actions">
                            <button class="button primary" :title="device.status?.ranging ? 'Stop ranging to start live calibration' : ''" :disabled="!connected || !device.status?.radio_ready || device.status?.standby || device.status?.ranging || calibration.phase === 'starting'" @click="beginAntennaCalibration">
                                <Antenna :size="15" /> {{ calibration.phase === 'starting' ? 'Starting…' : 'Start live calibration' }}
                            </button>
                        </div>
                    </div>

                    <template v-else>
                        <div class="calibration-live-grid">
                            <div class="panel calibration-reading">
                                <div class="calibration-card-top">
                                    <span>SMOOTHED DISTANCE</span>
                                    <span class="live-indicator" :class="{ paused: !calibration.processing }"><i></i> {{ calibration.processing ? 'LIVE' : 'PAUSED' }}</span>
                                </div>
                                <div class="calibration-reading-value">
                                    <strong>{{ calibration.averageMm === null ? '—' : (calibration.averageMm / 1000).toFixed(3) }}</strong>
                                    <span>m</span>
                                </div>
                                <div class="calibration-reading-foot">
                                    <span>Rolling average · last {{ CAL_WINDOW_SIZE }} valid samples</span>
                                    <b>{{ calibration.windowValues.length }} / {{ CAL_WINDOW_SIZE }}</b>
                                </div>
                                <div class="calibration-window-track"><span :style="{ width: `${calibration.windowValues.length / CAL_WINDOW_SIZE * 100}%` }"></span></div>
                                <div class="calibration-reading-meta">
                                    <div><span>Latest sample</span><b>{{ calibration.latestMm === null ? '—' : `${(calibration.latestMm / 1000).toFixed(3)} m` }}</b></div>
                                    <div><span>Valid ranges received</span><b>{{ calibration.validSamples }}</b></div>
                                    <div><span>Failed attempts</span><b>{{ calibration.failedSamples }}</b></div>
                                </div>
                            </div>

                            <div class="panel calibration-adjustment">
                                <div class="panel-heading">
                                    <div>
                                        <h3>Antenna delay</h3>
                                        <p>TX and RX are adjusted together.</p>
                                    </div>
                                    <Settings2 :size="17" />
                                </div>
                                <div class="calibration-delay-value">
                                    <input class="calibration-delay-number" type="number" min="0" max="65535" step="1" v-model="calibrationDelayInput" @change="commitCalibrationDelayInput" @keydown.enter.prevent="commitCalibrationDelayInput" :disabled="calibration.operationBusy" aria-label="Antenna delay in ticks" />
                                    <span>ticks</span>
                                </div>
                                <input class="calibration-delay-slider" type="range" min="0" max="65535" step="1" :value="calibration.requestedDelay" @input="setCalibrationDelay" :disabled="calibration.operationBusy" aria-label="TX and RX antenna delay" />
                                <div class="calibration-slider-labels"><span>0</span><span>65,535</span></div>
                                <div class="calibration-delay-offset">
                                    <div>
                                        <span>Nominal range shift vs. default (16,385)</span>
                                        <b>{{ calibrationRangeShiftMeters >= 0 ? '+' : '−' }}{{ Math.abs(calibrationRangeShiftMeters).toFixed(3) }} m</b>
                                    </div>
                                    <small>{{ (calibration.requestedDelay - DEFAULT_ANTENNA_DELAY) > 0 ? '+' : '' }}{{ calibration.requestedDelay - DEFAULT_ANTENNA_DELAY }} ticks · 4.692 mm per tick · positive means the reported range increases</small>
                                </div>
                                <div class="calibration-delay-details">
                                    <div><span>Applied to device</span><b>{{ calibration.appliedTx.toLocaleString() }} / {{ calibration.appliedRx.toLocaleString() }}</b></div>
                                    <div><span>Saved in NVS</span><b>{{ calibration.savedTx.toLocaleString() }} / {{ calibration.savedRx.toLocaleString() }}</b></div>
                                </div>
                                <div class="calibration-pending" v-if="calibration.delayPending">
                                    New delay will be applied after the current range completes.
                                </div>
                                <div class="calibration-saved" v-else-if="calibration.isSaved">
                                    <BadgeCheck :size="14" /> Current delay matches the saved NVS values.
                                </div>
                                <div class="calibration-unsaved" v-else>Current delay is active in RAM but has not been saved.</div>
                            </div>
                        </div>

                        <div class="panel calibration-actions-panel">
                            <div class="calibration-actions-copy">
                                <b>{{ calibration.isSaved ? 'Antenna delays saved' : 'Unsaved antenna-delay adjustment' }}</b>
                                <p>{{ calibration.message || 'Move the slider and use the live distance reading as feedback.' }}</p>
                            </div>
                            <div class="calibration-actions">
                                <button class="button primary" :disabled="calibration.operationBusy || !connected" @click="saveAntennaCalibration">
                                    <Save :size="15" /> {{ calibration.operationBusy && calibration.phase === 'saving' ? 'Saving…' : 'Save' }}
                                </button>
                                <button class="button secondary" :disabled="calibration.operationBusy || !connected" @click="resetAntennaCalibration">
                                    <RotateCcw :size="15" /> {{ calibration.operationBusy && calibration.phase === 'resetting' ? 'Resetting…' : 'Reset' }}
                                </button>
                            </div>
                        </div>
                        <div v-if="calibration.connectionUnstable" class="alert warning calibration-alert" role="status" aria-live="polite"><CircleAlert :size="16"/><span>{{ calibration.connectionWarning }}</span></div>
                        <div v-if="calibration.error" class="alert error calibration-alert" role="alert"><CircleAlert :size="16"/><span>{{ calibration.error }}</span></div>
                    </template>

                    <div class="hint-block">
                        <CircleHelp :size="17"/><span>The moving average uses up to {{ CAL_WINDOW_SIZE }} successful range measurements. Failed ranges are ignored. Slider changes are applied between measurements. Save writes only the current TX/RX antenna delays to NVS; Reset reloads those two values from NVS. Other saved device settings are not changed.</span>
                    </div>
                </section>

                <section v-else-if="active === 'configuration'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">DEVICE SETTINGS</div>
                            <h1>Configuration</h1>
                            <p>Configure shared node identity, UWB ranging and Bluetooth transport. Settings are saved together.</p>
                        </div>
                        <div class="heading-actions">
                            <span v-if="dirty" class="unsaved"><span class="dirty-dot" /> Unsaved edits</span>
                            <button class="button secondary" :disabled="!connected || savingConfig" @click="refreshAll">
                                <RefreshCw :size="15" /> Reload
                            </button>
                            <button class="button primary" :title="device.status?.ranging ? 'Stop ranging before saving configuration' : ble.runtime.connected ? 'Disconnect Bluetooth before saving configuration' : ''" :disabled="!connected || !dirty || savingConfig || device.status?.ranging || ble.runtime.connected" @click="saveSettings">
                                <Save :size="15" :class="{ spinning: savingConfig }" /> {{ savingConfig ? 'Saving…' : 'Save configuration' }}
                            </button>
                        </div>
                    </div>
                    <fieldset class="config-fields" :disabled="savingConfig">
                    <div class="config-sections">
                        <!-- Shared node identity: protocol-independent addressing. -->
                        <div class="panel form-panel config-section-panel config-identity-section">
                            <div class="form-title config-section-title">
                                <div class="form-icon"><Cpu /></div>
                                <div>
                                    <h3>Node identity</h3>
                                    <p>Shared device identity used by UWB and Bluetooth</p>
                                </div>
                            </div>
                            <div class="config-identity-fields">
                                <label>Device ID <span class="field-help">1–65534</span>
                                    <input type="number" min="1" max="65534" v-model.number="draft.deviceId" />
                                    <small>Short address identifying this node. Used by UWB target addressing and BLE peer identity; it must not match a remote peer.</small>
                                </label>
                            </div>
                        </div>

                        <!-- UWB settings grouped by network, radio, schedule and target list. -->
                        <div class="panel form-panel config-section-panel">
                            <div class="form-title config-section-title">
                                <div class="form-icon"><Waves /></div>
                                <div>
                                    <h3>UWB ranging</h3>
                                    <p>DWM3000 network, radio calibration and ranging behavior</p>
                                </div>
                            </div>
                            <div class="config-subsection-grid">
                                <div class="config-subsection">
                                    <div class="config-subheading">
                                        <h4>Network and role</h4>
                                        <p>UWB network addressing and saved boot behavior</p>
                                    </div>
                                    <div class="field-grid">
                                        <label>UWB ranging role
                                            <select v-model.number="draft.role">
                                                <option :value="0">Idle</option>
                                                <option :value="1">Initiator / Sender</option>
                                                <option :value="2">Responder / Receiver</option>
                                            </select>
                                            <small>Saved boot role. Current live RAM role: {{ roleName }}. Ranging test changes are temporary.</small>
                                        </label>
                                        <label>PAN ID <span class="field-help">HEX · 16-bit</span>
                                            <div class="input-prefix"><span>0x</span><input maxlength="4" v-model="draft.panId"
                                                @input="draft.panId = draft.panId.replace(/[^0-9a-f]/gi, '').slice(0, 4).toUpperCase()" /></div>
                                            <small>UWB network identifier. Must match compatible ranging peers.</small>
                                        </label>
                                    </div>
                                    <label class="full-field auto-start-field">
                                        <span class="auto-start-title"><input type="checkbox" v-model="draft.autoStart" /> Automatically start ranging after boot</span>
                                        <small>On boot, an Initiator starts its saved target schedule; a Responder starts listening.</small>
                                    </label>
                                </div>

                                <div class="config-subsection">
                                    <div class="config-subheading">
                                        <h4>Radio parameters</h4>
                                        <p>UWB PHY channel and antenna-delay calibration</p>
                                    </div>
                                    <label>UWB channel</label>
                                    <div class="choice-row">
                                        <button :class="{ chosen: draft.channel === 5 }" @click="draft.channel = 5"><b>Channel 5</b><span>6.5 GHz</span></button>
                                        <button :class="{ chosen: draft.channel === 9 }" @click="draft.channel = 9"><b>Channel 9</b><span>8.0 GHz</span></button>
                                    </div>
                                    <div class="field-grid">
                                        <label>TX antenna delay <span class="field-help">ticks · uint16</span>
                                            <input type="number" min="0" max="65535" v-model.number="draft.tx" />
                                            <small>Default firmware value: 16385</small>
                                        </label>
                                        <label>RX antenna delay <span class="field-help">ticks · uint16</span>
                                            <input type="number" min="0" max="65535" v-model.number="draft.rx" />
                                            <small>Default firmware value: 16385</small>
                                        </label>
                                    </div>
                                </div>

                                <div class="config-subsection">
                                    <div class="config-subheading">
                                        <h4>Ranging schedule</h4>
                                        <p>Periodic measurement timing and retry policy</p>
                                    </div>
                                    <div class="field-grid">
                                        <label>Target period <span class="field-help">20–60000 ms</span>
                                            <input type="number" min="20" max="60000" v-model.number="draft.period" />
                                            <small>Requested period for each target.</small>
                                        </label>
                                        <div class="schedule-preview">
                                            <span>Saved requested period</span><b>{{ draft.period }} ms / target</b>
                                            <small>Loaded from NVS; live test scheduling may temporarily differ.</small>
                                        </div>
                                        <label>UWB transaction retries <span class="field-help">0–5</span>
                                            <input type="number" min="0" max="5" v-model.number="draft.retries" />
                                            <small>Additional initiator attempts after a failed transaction.</small>
                                        </label>
                                        <label>Transaction timeout <span class="field-help">50–5000 ms</span>
                                            <input type="number" min="50" max="5000" v-model.number="draft.timeout" />
                                            <small>Maximum wait for a ranging transaction.</small>
                                        </label>
                                    </div>
                                </div>

                                <div class="config-subsection">
                                    <div class="config-subheading">
                                        <h4>UWB target list</h4>
                                        <p>Short-address destinations or responder allow-list · maximum 16 IDs</p>
                                    </div>
                                    <label>Target IDs <span class="field-help">comma separated</span>
                                        <input v-model="draft.targets" placeholder="e.g. 2, 3, 12" />
                                        <small>Initiator: destinations to range. Responder: permitted initiator IDs. An empty responder list accepts any compatible peer.</small>
                                    </label>
                                    <div class="target-chips">
                                        <span v-for="id in draft.targets.split(',').filter(x => x.trim())" :key="id">{{ id.trim() }}</span>
                                        <span v-if="!draft.targets.trim()" class="muted-chip">No targets configured</span>
                                    </div>
                                </div>
                            </div>
                        </div>

                        <!-- Bluetooth is a peer transport, separate from UWB ranging. -->
                        <div class="panel form-panel config-section-panel config-ble-section">
                            <div class="form-title config-section-title">
                                <div class="form-icon"><Bluetooth /></div>
                                <div>
                                    <h3>Bluetooth transport</h3>
                                    <p>BLE discovery, connection management and reliable message delivery</p>
                                </div>
                            </div>
                            <div v-if="ble.supported" class="config-subsection-grid ble-config-grid">
                                <div class="config-subsection">
                                    <div class="config-subheading">
                                        <h4>BLE role and discovery</h4>
                                        <p>Select how this node participates in Bluetooth connections</p>
                                    </div>
                                    <div class="field-grid">
                                        <label>BLE operating role
                                            <select v-model.number="bleDraft.mode">
                                                <option :value="0">Idle · disabled</option>
                                                <option :value="1">HUB · Central / scans and connects</option>
                                                <option :value="2">PERIPHERAL · advertises and accepts a connection</option>
                                            </select>
                                            <small>HUB initiates connections. PERIPHERAL advertises as UWB-XXXX using this node's Device ID.</small>
                                        </label>
                                        <label>Scan window <span class="field-help">10 ms to scan interval</span>
                                            <input type="number" min="10" :max="bleDraft.scanIntervalMs" step="1" v-model.number="bleDraft.scanWindowMs" />
                                            <small>Must not exceed the scan interval.</small>
                                        </label>
                                        <label>Advertising interval <span class="field-help">20–10240 ms</span>
                                            <input type="number" min="20" max="10240" step="1" v-model.number="bleDraft.advertisingIntervalMs" />
                                            <small>Used in PERIPHERAL mode.</small>
                                        </label>
                                        <label>Scan interval <span class="field-help">10–10240 ms</span>
                                            <input type="number" min="10" max="10240" step="1" v-model.number="bleDraft.scanIntervalMs" />
                                            <small>Used in HUB mode.</small>
                                        </label>
                                    </div>
                                </div>

                                <div class="config-subsection">
                                    <div class="config-subheading">
                                        <h4>Automatic connections and message delivery</h4>
                                        <p>Connections and recovery are managed by the node while BLE is enabled</p>
                                    </div>
                                    <div class="callout info">
                                        <Bluetooth :size="16" />
                                        <div><b>Automatic connection management</b><p>HUB nodes continuously discover and reconnect to peers. Peripheral nodes advertise and accept permitted peers. This remains active while Bluetooth is enabled.</p></div>
                                    </div>
                                    <label class="full-field auto-start-field">
                                        <span class="auto-start-title"><input type="checkbox" v-model="bleDraft.autoStart" /> Automatically start Bluetooth after boot</span>
                                        <small>Start the configured BLE role automatically after the next boot. This preference is independent of the Enable/Disable Bluetooth control and does not change the current radio state.</small>
                                    </label>
                                    <div class="field-grid">
                                        <label>Reconnect interval <span class="field-help">100–60000 ms</span>
                                            <input type="number" min="100" max="60000" step="1" v-model.number="bleDraft.retryIntervalMs" />
                                            <small>Delay before the next automatic connection attempt.</small>
                                        </label>
                                        <label>Connection timeout <span class="field-help">100–30000 ms</span>
                                            <input type="number" min="100" max="30000" step="1" v-model.number="bleDraft.connectionTimeoutMs" />
                                            <small>Maximum time allowed for a connection attempt.</small>
                                        </label>
                                        <label>BLE message retries <span class="field-help">0–5 additional attempts</span>
                                            <input type="number" min="0" max="5" step="1" v-model.number="bleDraft.maxRetries" />
                                            <small>Applied to fragment and final-result delivery.</small>
                                        </label>
                                        <label>Receive-consumption timeout <span class="field-help">100–2000 ms</span>
                                            <input type="number" min="100" max="2000" step="1" v-model.number="bleDraft.rxMessageTimeoutMs" />
                                            <small>Unread complete messages expire after this interval.</small>
                                        </label>
                                    </div>
                                </div>

                                <div class="config-subsection config-subsection-wide">
                                    <div class="peer-config-heading">
                                        <div class="config-subheading">
                                            <h4>BLE peer list</h4>
                                            <p>Peer allow-list · maximum 8 unique 16-bit IDs; an empty list means open mode</p>
                                        </div>
                                        <span class="tag neutral">{{ blePeerTokens.length }} / 8</span>
                                    </div>
                                    <label>Peer IDs <span class="field-help">comma separated</span>
                                        <input v-model="blePeersInput" placeholder="e.g. 2, 3, 12" />
                                        <small>HUB mode connects to these IDs. Peripheral mode accepts these IDs. Leave the list empty to discover or accept any valid peer.</small>
                                    </label>
                                    <div class="target-chips">
                                        <span v-for="id in blePeerTokens" :key="id">{{ id.trim() }}</span>
                                        <span v-if="!blePeersInput.trim()" class="muted-chip">Open mode · all valid peer IDs</span>
                                    </div>
                                </div>
                            </div>
                            <div v-else class="callout warning ble-unavailable-callout">
                                <Bluetooth :size="17" />
                                <div><b>Bluetooth configuration unavailable</b><p>The connected firmware does not expose the BLE configuration extension. Flash the BLE-enabled firmware build to manage Bluetooth from this application. Existing UWB settings remain available.</p></div>
                            </div>
                        </div>
                    </div>
                    </fieldset>
                    <div class="bottom-actions">
                        <button class="button danger-ghost" :disabled="!connected || savingConfig" @click="resetConfig">
                            <RotateCcw :size="15" /> Reset compiled defaults
                        </button>
                        <span v-if="device.status?.ranging" class="hint">Stop ranging before saving configuration.</span>
                    </div>
                </section>

                <section v-else-if="active === 'bluetooth'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">WIRELESS MESSAGING</div>
                            <h1>Bluetooth</h1>
                            <p>Configure the peer network, enable autonomous connections and monitor up to eight simultaneous links.</p>
                        </div>
                        <div class="heading-actions">
                            <button class="button secondary" :disabled="!connected || !ble.supported || ble.loading" @click="refreshBluetooth"><RefreshCw :size="15" :class="{ spinning: ble.loading }" /> Refresh</button>
                            <button class="button" :class="ble.runtime.enabled ? 'danger-ghost' : 'primary'" :disabled="!connected || !ble.supported || ble.action !== ''" @click="toggleBle"><Power :size="15" /> {{ ble.action ? 'Updating…' : ble.runtime.enabled ? 'Disable Bluetooth' : 'Enable Bluetooth' }}</button>
                        </div>
                    </div>
                    <div v-if="!ble.supported" class="callout warning">
                        <Bluetooth :size="17" /><div><b>BLE is not available on this firmware</b><p>This app requires firmware that reports the BLE extension in GET_STATUS, GET_CONFIG and GET_SAVED_CONFIG. UWB ranging remains usable with older firmware.</p></div>
                    </div>
                    <template v-else>
                        <div class="metric-grid ble-status-grid">
                            <div class="metric-card"><div class="metric-top"><span>CONFIGURED MODE</span><Bluetooth /></div><div class="metric-value">{{ bleModeName(ble.runtimeConfig.mode) }}</div><div class="metric-foot">Saved mode: {{ bleModeName(ble.savedConfig.mode) }}</div></div>
                            <div class="metric-card"><div class="metric-top"><span>RADIO STATE</span><Activity /></div><div class="metric-value" :class="ble.runtime.enabled ? 'good' : ''">{{ !ble.runtime.enabled ? 'Stopped' : ble.runtime.advertising ? 'Advertising' : ble.runtime.scanning ? 'Scanning' : 'Enabled' }}</div><div class="metric-foot">{{ ble.runtimeConfig.mode === 1 ? 'HUB / Central' : ble.runtimeConfig.mode === 2 ? 'Peripheral' : 'Mode idle' }}</div></div>
                            <div class="metric-card"><div class="metric-top"><span>ACTIVE LINKS</span><Link2 /></div><div class="metric-value" :class="bleConnectedCount ? 'good' : ''">{{ bleConnectedCount }} / 8</div><div class="metric-foot">{{ ble.runtime.connected ? 'Peer connections are maintained automatically' : 'No active peer links' }}</div></div>
                            <div class="metric-card"><div class="metric-top"><span>REPRESENTATIVE MTU</span><Database /></div><div class="metric-value">{{ ble.runtime.mtu }} B</div><div class="metric-foot">Each connected peer has its own negotiated MTU</div></div>
                        </div>
                        <div class="two-col ble-main-grid">
                            <div class="panel">
                                <div class="panel-heading"><div><h3>Peer connections</h3><p>Live state for configured and discovered peer identities</p></div><span class="tag neutral">{{ bleConnectedCount }} / 8 connected</span></div>
                                <div class="state-line"><span>Peer policy</span><b>{{ ble.runtimeConfig.peers.length ? 'Allow-list' : 'Open mode' }}</b></div>
                                <div v-if="bleVisiblePeerIds.length" class="table-wrap">
                                    <table><thead><tr><th>DEVICE ID</th><th>STATE</th><th>MTU</th><th>LAST CONNECTION FAILURE</th><th></th></tr></thead>
                                        <tbody><tr v-for="peerId in bleVisiblePeerIds" :key="peerId">
                                            <td><b>{{ peerId }}</b><small class="mono">{{ hex(peerId) }}</small></td>
                                            <td><span class="tag" :class="getBlePeerStatus(peerId)?.connected ? 'good' : [2, 4, 5, 6].includes(getBlePeerStatus(peerId)?.state ?? 0) ? 'warning' : 'neutral'">{{ getBlePeerStatus(peerId) ? blePeerStateName(getBlePeerStatus(peerId)?.state ?? 0) : 'Not checked' }}</span></td>
                                            <td>{{ getBlePeerStatus(peerId)?.connected ? getBlePeerStatus(peerId)?.mtu : '—' }}</td>
                                            <td><small v-if="bleConnectionFailureText(getBlePeerStatus(peerId))" class="ble-error-detail">{{ bleConnectionFailureText(getBlePeerStatus(peerId)) }}</small><span v-else>—</span></td>
                                            <td class="ble-peer-actions"><button class="icon-button mini" title="Refresh peer status" :disabled="ble.loading" @click="refreshBlePeerStatus(peerId)"><RefreshCw :size="13" /></button></td>
                                        </tr></tbody>
                                    </table>
                                </div>
                                <div v-else class="empty-state">
                                    <span v-if="ble.runtimeConfig.peers.length">Waiting for configured peers to be discovered or connect.</span>
                                    <span v-else>Open mode is active. Any valid peer can connect; connected peers will appear here automatically.</span>
                                    <button class="text-button" @click="navigate('configuration')">Manage peer policy <ArrowUpFromLine :size="13" /></button>
                                </div>
                                <div class="panel-inline-actions"><button class="button secondary small" @click="navigate('configuration')">Manage configuration</button></div>
                                <div v-if="ble.discoveredPeers.length" class="discovered-line"><b>Observed this session:</b><span v-for="id in ble.discoveredPeers" :key="id" class="tag good">{{ id }}</span></div>
                            </div>
                            <div class="panel ble-quick-test">
                                <div class="panel-heading"><div><h3>Message test</h3><p>Send a unicast text payload to a selected connected peer</p></div><Send /></div>
                                <label>Destination peer ID<input type="number" min="1" max="65534" v-model="bleMessagePeerId" :placeholder="bleConnectedCount ? 'Enter a connected peer ID' : 'Waiting for a peer link'" /></label>
                                <label>Message payload<textarea v-model="bleMessageText" rows="4" maxlength="496" placeholder="Type a short test message…" /></label>
                                <div class="ble-compose-meta"><span>{{ bleMessageByteLength }} / 496 bytes (UTF-8)</span><span v-if="getBlePeerStatus(Number(bleMessagePeerId))?.connected" class="tag good">PEER LINK READY</span><span v-else class="tag neutral">PEER NOT CONNECTED</span></div>
                                <div class="panel-inline-actions"><button class="button primary" :disabled="!getBlePeerStatus(Number(bleMessagePeerId))?.connected || ble.sending || bleMessageByteLength === 0 || bleMessageByteLength > 496" @click="sendBleMessage"><Send :size="14" /> {{ ble.sending ? 'Sending…' : 'Send message' }}</button><button class="button secondary" :disabled="!getBlePeerStatus(Number(bleMessagePeerId))?.connected || ble.sending" @click="sendBleTestMessage">Send test payload</button></div>
                                <small class="hint">SEND_TO_PEER acceptance only means the local queue accepted the payload. “Delivered” is shown only after the remote host retrieves the complete message.</small>
                            </div>
                        </div>
                        <div class="two-col ble-message-grid">
                            <div class="panel">
                                <div class="panel-heading"><div><h3>Received messages</h3><p>Complete messages retrieved from the firmware receive queue</p></div><div class="panel-heading-actions"><Inbox :size="16" /><span class="tag neutral">{{ ble.inbox.length }}</span></div></div>
                                <div v-if="ble.inbox.length" class="ble-message-list">
                                    <article v-for="message in ble.inbox" :key="`${message.peerId}:${message.messageId}:${message.time}`" class="ble-message-item">
                                        <div class="ble-message-top"><b>Peer {{ message.peerId }} · Message {{ message.messageId }}</b><span class="tag" :class="message.state === 'Retrieved' ? 'good' : 'warning'">{{ message.state }}</span></div>
                                        <div class="ble-message-text">{{ message.text || '(No UTF-8 text content)' }}</div>
                                        <div class="ble-message-meta">{{ message.length }} bytes · {{ new Date(message.time).toLocaleTimeString() }}</div>
                                        <code>{{ message.hex || '—' }}</code>
                                    </article>
                                </div>
                                <div v-else class="empty-state">Waiting for a BLE_DATA_RECEIVED event. Incoming messages are fetched automatically.</div>
                            </div>
                            <div class="panel">
                                <div class="panel-heading"><div><h3>Outgoing messages</h3><p>Delivery status returned by the remote application</p></div><span class="tag neutral">{{ ble.outgoing.length }}</span></div>
                                <div v-if="ble.outgoing.length" class="table-wrap"><table><thead><tr><th>PEER</th><th>MESSAGE</th><th>SIZE</th><th>STATUS</th></tr></thead><tbody>
                                    <tr v-for="message in ble.outgoing" :key="`${message.peerId}:${message.messageId}`"><td>{{ message.peerId }}</td><td>{{ message.messageId }}</td><td>{{ message.length }} B</td><td><span class="tag" :class="message.state === 'Delivered' ? 'good' : message.state === 'Failed' ? 'bad' : 'warning'">{{ message.state }}</span><small v-if="message.error" class="ble-error-detail">{{ message.error }}</small></td></tr>
                                </tbody></table></div>
                                <div v-else class="empty-state">No outgoing BLE messages in this session.</div>
                            </div>
                        </div>
                        <div class="bottom-actions"><span class="hint">The boot auto-start preference is configured separately in Configuration. Runtime Enable/Disable does not change that saved preference. Connections recover automatically while enabled. Application messages are unicast to the selected peer ID; no broadcast is performed.</span><button class="button secondary" @click="clearBleMessages"><Trash2 :size="14" /> Clear message history</button></div>
                    </template>
                </section>

                <section v-else-if="active === 'ranging'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">RADIO VALIDATION</div>
                            <h1>Ranging test</h1>
                            <p>Set the node role, execute one-shot measurements or run the periodic ranging scheduler.
                            </p>
                        </div><span class="tag" :class="device.status?.ranging ? 'good' : 'neutral'">{{
                            device.status?.ranging ?
                            'RANGING ACTIVE':'STOPPED'}}</span>
                    </div>
                    <div class="panel">
                        <div class="panel-heading">
                            <div>
                                <h3>Operating role</h3>
                                <p>Role changes stop active ranging and cancel queued one-shot jobs. These controls change RAM only; they never write NVS.</p>
                            </div>
                        </div>
                        <div class="role-grid"><button :class="{ chosen: device.status?.role === 0 }"
                                @click="setRole(0)">
                                <div class="role-icon">
                                    <Power />
                                </div><b>Idle</b><span>Radio not ranging</span>
                            </button><button :class="{ chosen: device.status?.role === 1 }" @click="setRole(1)">
                                <div class="role-icon">
                                    <ArrowUpFromLine />
                                </div><b>Initiator / Sender</b><span>Poll configured target IDs</span>
                            </button><button :class="{ chosen: device.status?.role === 2 }" @click="setRole(2)">
                                <div class="role-icon">
                                    <ArrowDownToLine />
                                </div><b>Responder / Receiver</b><span>Listen for compatible peers</span>
                            </button></div>
                    </div>
                    <div class="two-col">
                        <div class="panel test-panel">
                            <div class="panel-heading">
                                <div>
                                    <h3>Continuous ranging</h3>
                                    <p>Uses the configured target list and schedule.</p>
                                </div>
                                <Activity />
                            </div>
                            <div class="state-line"><span>Current
                                    state</span><b>{{ device.status?.ranging ? 'Enabled' : 'Disabled' }}</b>
                            </div>
                            <div class="state-line"><span>Role</span><b>{{ roleName }}</b></div>
                            <div class="state-line"><span>Configured targets</span><b>{{ device.targets.length }}</b>
                            </div>
                            <div class="button-row"><button class="button primary"
                                    :disabled="!connected || device.status?.ranging || device.status?.role === 0 || !device.status?.radio_ready || (device.status?.role === 1 && device.targets.length === 0)"
                                    @click="startRange">
                                    <Zap :size="16" /> Start ranging
                                </button><button class="button secondary"
                                    :disabled="!connected || !device.status?.ranging" @click="stopRange">
                                    <Power :size="16" /> Stop
                                </button></div>
                            <p class="hint" v-if="device.status?.role === 1 && device.targets.length === 0">Initiator
                                mode
                                requires at least
                                one target ID before starting.</p>
                        </div>
                        <div class="panel test-panel">
                            <div class="panel-heading">
                                <div>
                                    <h3>One-shot range</h3>
                                    <p>Queue one measurement for a specific peer.</p>
                                </div>
                                <Zap />
                            </div><label>Target device ID<input type="number" v-model="oneShotTarget" min="1"
                                    max="65534" /></label>
                            <div class="button-row"><button class="button primary"
                                    :disabled="!connected || device.status?.role !== 1 || !device.status?.radio_ready || device.status?.standby"
                                    @click="rangeOnce">
                                    <Zap :size="16" /> Queue one-shot
                                </button></div>
                            <p class="hint">Requires initiator role and an available radio. Queue capacity: 8 jobs.</p>
                        </div>
                    </div>
                    <div class="panel recent-panel">
                        <div class="panel-heading">
                            <div>
                                <h3>Live measurement stream</h3>
                                <p>Range result and failure events received over USB</p>
                            </div><span class="tag neutral">{{ eventCount }} events</span>
                        </div>
                        <div v-if="measurements.length" class="table-wrap">
                            <table>
                                <thead>
                                    <tr>
                                        <th>TIME</th>
                                        <th>SEQ</th>
                                        <th>TARGET</th>
                                        <th>RANGE</th>
                                        <th>STATUS</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <tr v-for="m in measurements.slice(0, 12)" :key="`${m.seq}-${m.time}`">
                                        <td>{{ new Date(m.time).toLocaleTimeString() }}</td>
                                        <td>{{ m.seq }}</td>
                                        <td>{{ m.target }}</td>
                                        <td>{{ fmtRange(m.mm < 0 ? null : m.mm) }}</td>
                                        <td><span class="tag" :class="m.status === 0 ? 'good' : 'bad'">{{
                                                statusText(m.status) }}</span>
                                        </td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                        <div v-else class="empty-state">Waiting for ranging measurements.</div>
                    </div>
                </section>

                <section v-else-if="active === 'history'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">MEASUREMENT DATA</div>
                            <h1>Measurements & history</h1>
                            <p>Review retained ranging records from the device circular buffer (up to 256 records).</p>
                        </div>
                        <div class="heading-actions"><button class="button secondary" :disabled="!connected"
                                @click="loadHistory">
                                <Download :size="15" /> Load device history
                            </button><button class="button danger-ghost" :disabled="!connected" @click="clearHistory">
                                <Trash2 :size="15" /> Clear history
                            </button></div>
                    </div>
                    <div class="panel">
                        <div class="panel-heading">
                            <div>
                                <h3>Retained samples</h3>
                                <p>{{ measurements.length }} samples currently loaded into this view</p>
                            </div><span class="tag neutral">Max 256</span>
                        </div>
                        <div v-if="measurements.length" class="table-wrap">
                            <table>
                                <thead>
                                    <tr>
                                        <th>SAMPLE SEQ</th>
                                        <th>TARGET ID</th>
                                        <th>MONOTONIC TIME (µs)</th>
                                        <th>DISTANCE</th>
                                        <th>STATUS</th>
                                        <th>QUALITY FLAGS</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <tr v-for="m in measurements" :key="`${m.seq}-${m.time}`">
                                        <td>#{{ m.seq }}</td>
                                        <td>{{ m.target }}</td>
                                        <td class="mono">{{ m.mono ?? '—' }}</td>
                                        <td>{{ fmtRange(m.mm < 0 ? null : m.mm) }}</td>
                                        <td><span class="tag" :class="m.status === 0 ? 'good' : 'bad'">{{
                                                statusText(m.status) }}</span>
                                        </td>
                                        <td>{{ hex(m.quality ?? 0, 2) }}</td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>
                        <div v-else class="empty-state">
                            <Database :size="25" /><b>No measurement data loaded</b><span>Load retained history from the
                                connected node
                                or start a ranging test.</span>
                        </div>
                    </div>
                    <div class="hint-block">
                        <CircleHelp :size="17" /><span>History is fetched in pages of up to 17 records. The device
                            retains up to 256
                            records; if the cursor predates retained data, firmware reports an overrun.</span>
                    </div>
                </section>

                <section v-else-if="active === 'diagnostics'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">DEVICE HEALTH</div>
                            <h1>Diagnostics</h1>
                            <p>Inspect the UWB radio, USB protocol and Bluetooth messaging state to identify integration issues.</p>
                        </div><button class="button primary" :disabled="!connected" @click="refreshAll">
                            <RefreshCw :size="15" /> Refresh diagnostics
                        </button>
                    </div>
                    <div class="metric-grid four">
                        <div class="metric-card">
                            <div class="metric-top"><span>RANGES OK</span>
                                <BadgeCheck />
                            </div>
                            <div class="metric-value">{{ device.stats?.ok ?? '—' }}</div>
                            <div class="metric-foot">Successful DS-TWR results</div>
                        </div>
                        <div class="metric-card">
                            <div class="metric-top"><span>RANGE FAILURES</span>
                                <CircleAlert />
                            </div>
                            <div class="metric-value">{{ device.stats?.failed ?? '—' }}</div>
                            <div class="metric-foot">Failed or invalid results</div>
                        </div>
                        <div class="metric-card">
                            <div class="metric-top"><span>RX ERRORS</span>
                                <ArrowDownToLine />
                            </div>
                            <div class="metric-value">{{ device.stats?.rx ?? '—' }}</div>
                            <div class="metric-foot">Radio receive errors</div>
                        </div>
                        <div class="metric-card">
                            <div class="metric-top"><span>TX ERRORS</span>
                                <ArrowUpFromLine />
                            </div>
                            <div class="metric-value">{{ device.stats?.tx ?? '—' }}</div>
                            <div class="metric-foot">Radio transmit errors</div>
                        </div>
                    </div>
                    <div class="two-col">
                        <div class="panel">
                            <div class="panel-heading">
                                <div>
                                    <h3>Protocol & transport</h3>
                                    <p>Host and firmware error counters</p>
                                </div>
                                <Terminal />
                            </div>
                            <div class="detail-list">
                                <div><span>Protocol errors</span><b>{{ device.stats?.protocol ?? '—' }}</b></div>
                                <div><span>UART overflows</span><b>{{ device.stats?.overflows ?? '—' }}</b></div>
                                <div><span>Duplicate requests</span><b>{{ device.stats?.duplicates ?? '—' }}</b></div>
                                <div><span>Transaction timeouts</span><b>{{ device.stats?.timeouts ?? '—' }}</b></div>
                                <div><span>History overwrites</span><b>{{ device.stats?.overwrites ?? '—' }}</b></div>
                                <div><span>Malformed radio frames</span><b>{{ device.stats?.malformed ?? '—' }}</b></div>
                                <div><span>BLE events logged</span><b>{{ events.filter(item => /BLE/.test(item)).length }}</b></div>
                                <div><span>BLE messages received</span><b>{{ ble.inbox.length }}</b></div>
                                <div><span>BLE messages sent</span><b>{{ ble.outgoing.length }}</b></div>
                            </div>
                        </div>
                        <div class="panel">
                            <div class="panel-heading">
                                <div>
                                    <h3>Radio registers</h3>
                                    <p>Live diagnostic snapshot</p>
                                </div>
                                <Antenna />
                            </div>
                            <div class="detail-list">
                                <div><span>Current radio status</span><b class="mono">{{ hex(device.diagnostics?.radio
                                        ?? 0, 8) }}</b>
                                </div>
                                <div><span>Live system status</span><b class="mono">{{ hex(device.diagnostics?.live ??
                                        0, 8) }}</b>
                                </div>
                                <div><span>Radio state machine</span><b>{{ device.diagnostics?.state ?? '—' }}</b></div>
                                <div><span>Standby</span><b>{{ device.diagnostics?.standby ? 'IDLE_RC' : 'No' }}</b>
                                </div>
                                <div><span>Last error code</span><b>{{ device.lastError?.code ?? '—' }} ·
                                        {{ ['OK', 'BAD_ARGUMENT', 'UNKNOWN_COMMAND', 'BAD_STATE', 'BUSY',
                                            'NOT_SUPPORTED', 'TIMEOUT',
                                            'RADIO_FAILURE', 'RATE_UNACHIEVABLE', 'INTERNAL',
                                        'NOT_FOUND'][device.lastError?.code ?? 0]
                                        }}</b>
                                </div>
                                <div><span>Last error monotonic time</span><b>{{ device.lastError?.time ?? '—' }} µs</b>
                                </div>
                            </div>
                        </div>
                    </div>
                    <div class="panel advanced-panel">
                        <div class="panel-heading">
                            <div>
                                <h3>Protocol tools</h3>
                                <p>Direct access to maintenance and event commands</p>
                            </div>
                            <Terminal />
                        </div>
                        <div class="advanced-actions"><button class="button secondary" :disabled="!connected"
                                @click="pingNode">PING</button><button class="button secondary" :disabled="!connected"
                                @click="syncClock">Synchronize clock</button><button class="button secondary"
                                :disabled="!connected" @click="readAntenna">Read antenna delays</button><button
                                class="button secondary" :disabled="!connected" @click="readLast">Get latest per
                                target</button><button class="button secondary" :disabled="!connected"
                                @click="subscribeAll">Subscribe
                                all events</button><button class="button secondary" :disabled="!connected"
                                @click="unsubscribeAll">Unsubscribe all</button></div>
                        <div class="cancel-row"><label>Queued job ID<input type="number" min="1" max="65535"
                                    v-model="cancelJobId" /></label><button class="button danger-ghost"
                                :disabled="!connected" @click="cancelJob">Cancel operation</button></div>
                        <div class="advanced-output"><span>LAST COMMAND RESULT</span><code>{{ advancedOutput }}</code>
                        </div>
                    </div>
                    <div class="panel advanced-panel">
                        <div class="panel-heading">
                            <div>
                                <h3>Recent device events</h3>
                                <p>Events decoded from the subscribed USB stream</p>
                            </div><span class="tag neutral">{{ events.length }} logged</span>
                        </div>
                        <div v-if="events.length" class="event-log">
                            <div v-for="(item, index) in events.slice(0, 10)" :key="index">{{ item }}</div>
                        </div>
                        <div v-else class="empty-state">No device events received.</div>
                    </div>
                    <div class="bottom-actions"><span class="hint">Counters are maintained by the firmware and reset on
                            reboot or
                            clear.</span><button class="button secondary" :disabled="!connected" @click="clearStats">
                            <Trash2 :size="15" /> Clear counters
                        </button></div>
                </section>

                <section v-else-if="active === 'system'">
                    <div class="page-heading">
                        <div>
                            <div class="eyebrow">POWER & MAINTENANCE</div>
                            <h1>Device controls</h1>
                            <p>Manage UWB radio standby, Bluetooth availability, device sleep and maintenance actions.</p>
                        </div>
                    </div>
                    <div class="panel control-row">
                        <div class="control-icon">
                            <Antenna />
                        </div>
                        <div class="control-copy">
                            <h3>UWB radio standby</h3>
                            <p>Stops ranging and places the DWM3000 in IDLE_RC standby. This is not true DWM3000 deep
                                sleep.</p><span class="tag" :class="device.status?.standby ? 'warning' : 'good'">{{
                                    device.status?.standby ? 'IN STANDBY':'AWAKE'}}</span>
                        </div>
                        <div class="control-actions"><button class="button secondary"
                                :disabled="!connected || device.status?.standby" @click="sleepRadio">Enter
                                standby</button><button class="button primary"
                                :disabled="!connected || !device.status?.standby" @click="wakeRadio">Wake radio</button>
                        </div>
                    </div>
                    <div class="panel control-row">
                        <div class="control-icon">
                            <Power />
                        </div>
                        <div class="control-copy">
                            <h3>ESP32 timed deep sleep</h3>
                            <p>Stops ranging and enters timer-woken ESP32 deep sleep. UWB and Bluetooth are unavailable while the ESP32 sleeps; UART cannot wake it. USB reconnects after timer wake.</p>
                        </div>
                        <div class="control-actions"><button class="button secondary" :disabled="!connected"
                                @click="systemSleep">Configure sleep timer</button></div>
                    </div>
                    <div class="panel control-row">
                        <div class="control-icon">
                            <RotateCcw />
                        </div>
                        <div class="control-copy">
                            <h3>Reboot node</h3>
                            <p>Send the reboot command. The response is acknowledged before the ESP32 restarts.</p>
                        </div>
                        <div class="control-actions"><button class="button secondary" :disabled="!connected"
                                @click="reboot">Reboot
                                device</button></div>
                    </div>
                    <div class="panel control-row destructive">
                        <div class="control-icon">
                            <Trash2 />
                        </div>
                        <div class="control-copy">
                            <h3>Reset configuration</h3>
                            <p>Clear saved UWB and Bluetooth configuration, peer lists and targets, restore compiled defaults, and reinitialize the radio. Both operating roles return to idle.</p>
                        </div>
                        <div class="control-actions"><button class="button danger" :disabled="!connected"
                                @click="resetConfig">Reset
                                device config</button></div>
                    </div>
                    <div class="hint-block">
                        <CircleHelp :size="17" /><span>The saved role and auto-start policy are restored from NVS at boot. Auto-start is controlled in Configuration; Ranging test role changes are temporary.</span>
                    </div>
                </section>
            </div>
            <footer class="footer"><span><span class="dot" :class="{ online: connected }" /> {{ connected ? 'USB CDC session active':'Waiting for USB device'}}</span><span>Protocol spec v2.0 <i>·</i> wire v1 <i>·</i> USB CDC 115200 <i>·</i> D3KM
                    0.2.0</span>
            </footer>
        </main>
    </div>
</template>
