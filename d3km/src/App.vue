<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue';
import { invoke } from '@tauri-apps/api/core';
import { listen, type UnlistenFn } from '@tauri-apps/api/event';
import {
    Activity,
    Antenna,
    ArrowDownToLine,
    ArrowUpFromLine,
    BadgeCheck,
    Bluetooth,
    Cable,
    ChartNoAxesCombined,
    CircleAlert,
    CircleHelp,
    Clock3,
    Cpu,
    Database,
    Download,
    History,
    Inbox,
    LayoutDashboard,
    Link2,
    Plug,
    Power,
    RefreshCw,
    RotateCcw,
    Save,
    Send,
    Settings2,
    ShieldCheck,
    Terminal,
    Trash2,
    Unplug,
    Waves,
    Zap,
} from 'lucide-vue-next';

const OP = {
    // Common commands, diagnostics, subscriptions, and time sync.
    PING: 0x01, GET_INFO: 0x02, GET_STATUS: 0x03, GET_STATISTICS: 0x04,
    CLEAR_STATISTICS: 0x05, GET_LAST_ERROR: 0x06, GET_DIAGNOSTICS: 0x07,
    GET_TRACE: 0x08, SUBSCRIBE: 0x09, UNSUBSCRIBE: 0x0A, TIME_SYNC: 0x0B,

    // Configuration and antenna calibration.
    GET_CONFIG: 0x10, SET_CONFIG: 0x11, RESET_CONFIG: 0x12,
    GET_SAVED_CONFIG: 0x13, APPLY_SAVE_CONFIG: 0x14,
    GET_ANTENNA_DELAYS: 0x15, SET_ANTENNA_DELAYS: 0x16, SAVE_ANTENNA_DELAYS: 0x17,

    // UWB control and ranging.
    SET_ROLE: 0x20, GET_TARGETS: 0x21, SET_TARGETS: 0x22,
    GET_UWB_SCHEDULE: 0x23, SET_UWB_SCHEDULE: 0x24,
    START_RANGING: 0x25, STOP_RANGING: 0x26, RANGE_ONCE: 0x27,
    CANCEL_OPERATION: 0x28, UWB_SLEEP: 0x29, UWB_WAKE: 0x2A,

    // UWB measurements and history.
    GET_LAST_RANGE: 0x30, GET_HISTORY: 0x31,

    // System power and restart.
    SYSTEM_SLEEP: 0x40, REBOOT: 0x41,

    // BLE peer management and messaging.
    GET_PEERS: 0x50, SET_PEERS: 0x51, START_BLE: 0x52,
    STOP_BLE: 0x53, GET_PEER_STATUS: 0x54, SEND_TO_PEER: 0x55,
    GET_RECEIVED_DATA: 0x56
} as const;

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
const connected = ref(false),
    connecting = ref(false),
    ports = ref<Port[]>([]),
    selectedPort = ref(''),
    active = ref('overview'),
    busy = ref(false),
    savingConfig = ref(false),
    savingStep = ref(''),
    lastRefresh = ref('—'),
    error = ref(''),
    notice = ref(''),
    events = ref<string[]>([]),
    measurements = ref<any[]>([]),
    eventCount = ref(0);

type CalJobResult = { target: number; status: number; mm: number | null };
type BleDiagnosticRecord = {
    uptimeMs: number; code: number; peerId: number; reason: number; stage: number;
    detail: number; activeLinks: number; stateFlags: number;
};
const CAL_WINDOW_SIZE = 100;
const DEFAULT_ANTENNA_DELAY = 16385;
// One DW3000 device-time unit converted to one-way propagation distance.
const DW3000_TICK_METERS = 299792458 / (499.2e6 * 128);
const calibration = reactive({
    phase: 'idle' as 'idle' | 'starting' | 'live' | 'saving' | 'resetting' | 'error',
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
let poller: number | undefined, eventDrainTimer: number | undefined, unlisten: UnlistenFn | undefined, polling = false, missingPortChecks = 0, losingConnection = false;
let lastBleRefreshAt = 0;
let lastTraceRefreshAt = 0;
let commandInFlight = false;
let eventDrainBusy = false;
const priorityCommandQueue: Array<() => Promise<void>> = [];
const normalCommandQueue: Array<() => Promise<void>> = [];

function runNextDeviceCommand() {
    if (commandInFlight) return;
    const next = priorityCommandQueue.shift() ?? normalCommandQueue.shift();
    if (!next) return;
    commandInFlight = true;
    void next();
}
function queueDeviceCommand<T>(operation: () => Promise<T>, priority = false): Promise<T> {
    return new Promise<T>((resolve, reject) => {
        const run = async (): Promise<void> => {
            try {
                resolve(await operation());
            } catch (e) {
                reject(e);
            } finally {
                commandInFlight = false;
                runNextDeviceCommand();
            }
        };
        (priority ? priorityCommandQueue : normalCommandQueue).push(run);
        runNextDeviceCommand();
    });
}
const device = reactive<any>({
    info: null, status: null, stats: null,
    // config/savedTargets always describe NVS; runtimeConfig/schedule/targets describe RAM.
    config: { deviceId: 1, panId: 0xDECA, channel: 5, tx: 16385, rx: 16385, period: 100, timeout: 120, retries: 1, role: 0, autoStart: false },
    savedTargets: [] as number[], runtimeConfig: null, schedule: null, targets: [] as number[], diagnostics: null, lastError: null
});
const draft = reactive({ deviceId: 1, panId: 'DECA', channel: 5, tx: 16385, rx: 16385, period: 100, timeout: 120, retries: 1, role: 0, targets: '', autoStart: false });
type UnifiedTraceRecord = { sequence: number; source: number; uptimeMs: number; text: string };
const traceRecords = ref<UnifiedTraceRecord[]>([]);
const traceFetchedAt = ref('');
const traceError = ref('');
const traceBusy = ref(false);
const traceEventName = (n: number) => ({ 1: 'dwt_starttx() failed', 2: 'dwt_rxenable() failed', 3: 'TXFRS completion timeout', 4: 'DW3000 RX error bits' } as Record<number, string>)[n] ?? `Unknown event ${n}`;
const radioFunctionName = (n: number) => ({ 0xe0: 'POLL', 0xe1: 'RESP', 0xe2: 'FINAL', 0xe3: 'REPORT' } as Record<number, string>)[n] ?? hex(n, 2);
const radioStateName = (n: number) => ({ 0: 'READY', 1: 'RX_LISTEN', 2: 'WAIT_RESP', 3: 'WAIT_FINAL' } as Record<number, string>)[n] ?? `State ${n}`;
function decodeTrace(payload: number[]): UnifiedTraceRecord[] {
    if (!payload.length) throw new Error('GET_TRACE returned an empty payload.');
    const count = payload[0];
    if (count > 48) throw new Error(`GET_TRACE returned too many records: ${count}.`);
    const sizes: Record<number, number> = { 0: 28, 1: 44, 2: 13 };
    const records: UnifiedTraceRecord[] = [];
    let offset = 1;
    for (let i = 0; i < count; i++) {
        if (offset + 6 > payload.length) throw new Error('GET_TRACE record header is truncated.');
        const source = payload[offset], sequence = get32(payload, offset + 1), size = payload[offset + 5];
        offset += 6;
        if (sizes[source] !== size || offset + size > payload.length) throw new Error(`GET_TRACE record ${i} has invalid type, size, or data length.`);
        const data = payload.slice(offset, offset + size); offset += size;
        const uptimeMs = get32(data, 0);
        let text = '';
        if (source === 0) {
            text = `${(uptimeMs / 1000).toFixed(1)}s · UWB radio · ${traceEventName(data[20])} · ${radioFunctionName(data[22])} · ${radioStateName(data[21])} · peer ${get16(data, 16)} · job ${get16(data, 18)} · SYS_STATUS ${hex(get32(data, 4), 8)} → ${hex(get32(data, 8), 8)} · rc ${hex(data[26], 2)}`;
        } else if (source === 1) {
            text = `${(uptimeMs / 1000).toFixed(1)}s · UWB TX detail · ${radioFunctionName(data[38])} · peer ${get16(data, 32)} · job ${get16(data, 34)} · ${failureHintName(data[43])} · SYS_STATUS ${hex(get32(data, 4), 8)} → ${hex(get32(data, 8), 8)} · SYS_STATE ${hex(get32(data, 12), 8)} → ${hex(get32(data, 16), 8)}`;
        } else {
            const rawDetail = get16(data, 9);
            const record: BleDiagnosticRecord = { uptimeMs, code: data[4], peerId: get16(data, 5), reason: data[7], stage: data[8], detail: rawDetail >= 0x8000 ? rawDetail - 0x10000 : rawDetail, activeLinks: data[11], stateFlags: data[12] };
            text = `${(uptimeMs / 1000).toFixed(1)}s · BLE · ${bleDiagnosticLine(record)}`;
        }
        records.push({ source, sequence, uptimeMs, text });
    }
    if (offset !== payload.length) throw new Error('GET_TRACE payload contains trailing bytes.');
    return records;
}
async function refreshTrace(silent = false) {
    if (!connected.value || traceBusy.value) return;
    traceBusy.value = true;
    try {
        traceRecords.value = decodeTrace(await cmd(OP.GET_TRACE));
        traceFetchedAt.value = new Date().toLocaleString();
        traceError.value = '';
        log(`GET_TRACE · ${traceRecords.value.length} record(s)`);
    } catch (e: any) {
        traceError.value = String(e);
        if (!silent) error.value = `Could not read device trace: ${String(e)}`;
    } finally { traceBusy.value = false; }
}
const failureHintName = (n: number) => ({ 0: 'Unknown / inconclusive', 1: 'HPDWARN observed after call', 2: 'TXERR state observed after call' } as Record<number, string>)[n] ?? `Unknown hint ${n}`;
function buildDebugReport() {
    return {
        reportFormat: 'D3KM device debug report',
        reportVersion: 3,
        protocolSpecVersion: '2.3',
        capturedAt: new Date().toISOString(),
        port: selectedPort.value,
        appProtocolVersion: 1,
        device: {
            info: device.info,
            status: device.status,
            savedConfig: device.config,
            savedTargets: device.savedTargets,
            runtimeConfig: device.runtimeConfig,
            runtimeSchedule: device.schedule,
            runtimeTargets: device.targets,
            stats: device.stats,
            diagnostics: device.diagnostics,
            lastError: device.lastError
        },
        bluetooth: {
            supported: ble.supported,
            runtime: { ...ble.runtime },
            runtimeConfig: cloneBleConfig(ble.runtimeConfig),
            savedConfig: cloneBleConfig(ble.savedConfig),
            peerStatuses: ble.peerStatuses.map(item => ({ ...item })),
            discoveredPeers: [...ble.discoveredPeers],
            receivedCount: ble.inbox.length,
            sentCount: ble.outgoing.length,
            incomingMessageMetadata: ble.inbox.map(({ peerId, messageId, length, time, state }) => ({ peerId, messageId, length, time, state })),
            outgoingMessageMetadata: ble.outgoing.map(({ peerId, messageId, length, time, state, error }) => ({ peerId, messageId, length, time, state, error }))
        },
        trace: {
            fetchedAt: traceFetchedAt.value,
            records: traceRecords.value
        },
        antennaCalibration: {
            phase: calibration.phase,
            referenceId: calibration.referenceId,
            requestedDelay: calibration.requestedDelay,
            appliedTx: calibration.appliedTx,
            appliedRx: calibration.appliedRx,
            savedTx: calibration.savedTx,
            savedRx: calibration.savedRx,
            averageMm: calibration.averageMm,
            latestMm: calibration.latestMm,
            windowSize: CAL_WINDOW_SIZE,
            validSamples: calibration.validSamples,
            failedSamples: calibration.failedSamples,
            isSaved: calibration.isSaved
        },
        recentEvents: events.value.slice(0, 40),
        recentMeasurements: measurements.value.slice(0, 20),
    };
}
async function copyDebugReport() {
    try { await navigator.clipboard.writeText(JSON.stringify(buildDebugReport(), null, 2)); notice.value = 'Debug report copied to clipboard'; log('Debug report copied'); }
    catch (e: any) { error.value = `Could not copy report: ${String(e)}`; }
}
function downloadDebugReport() {
    const blob = new Blob([JSON.stringify(buildDebugReport(), null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob); const a = document.createElement('a'); a.href = url; a.download = `d3km-debug-node-${device.info?.id ?? 'unknown'}-${new Date().toISOString().replace(/[:.]/g, '-')}.json`; a.click(); URL.revokeObjectURL(url);
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
    if (count > 16 || p.length < baseLength + 16) {
        throw new Error(`Invalid GET_SAVED_CONFIG payload: target count ${count}, length ${p.length}; complete BLE configuration is required.`);
    }
    const bleConfig = decodeBleConfigExtension(p, baseLength);
    return {
        deviceId: get16(p, 0), panId: get16(p, 2), channel: p[4], tx: get16(p, 5), rx: get16(p, 7),
        period: get32(p, 9), timeout: get16(p, 13), retries: p[15], role: p[16],
        targets: Array.from({ length: count }, (_, i) => get16(p, 18 + i * 2)),
        autoStart: p[18 + count * 2] === 1, bleConfig, bleConfigAvailable: true
    };
}
function readString(p: number[]) { return String.fromCharCode(...p) }
async function cmd(op: number, payload: number[] = [], priority = false) {
    if (!connected.value) throw new Error('Connect a device first');
    const task = queueDeviceCommand(
        () => invoke<{ payload: number[]; events: FrameEvent[] }>('send_command', { opcode: op, payload }),
        priority
    );
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
        const payload = await cmd(OP.GET_PEER_STATUS, u16(peerId));
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
        const [statusPayload, peersPayload] = await Promise.all([cmd(OP.GET_STATUS), cmd(OP.GET_PEERS)]);
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
        await cmd(OP.START_BLE);
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
        await cmd(OP.STOP_BLE);
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
        const response = await cmd(OP.SEND_TO_PEER, [...u16(peerId), ...u16(bytes.length), ...bytes]);
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
function nimbleStatusName(status: number) {
    return ({
        0x0008: 'Operation disabled at compile time',
        0x000d: 'Operation timed out (NimBLE host)',
        0x0208: 'Connection timeout (HCI)',
        0x020d: 'Connection rejected due to limited resources (HCI)',
        0x023e: 'Connection failed to be established (HCI)'
    } as Record<number, string>)[status] ?? 'Unmapped NimBLE status';
}
function bleConnectionDetailText(stage: number, detail: number) {
    if (detail === -1 || (detail === 0 && [5, 6, 9, 10, 12].includes(stage))) return 'detail unavailable';
    const hexDetail = `0x${(detail & 0xffff).toString(16).toUpperCase().padStart(4, '0')}`;
    if ([3, 4].includes(stage)) return `NimBLE status ${hexDetail} · ${nimbleStatusName(detail & 0xffff)}`;
    if (stage === 8) return `reported peer ID ${hexDetail}`;
    if (stage === 11) return `validation code ${detail}`;
    return `detail ${hexDetail}`;
}
function bleDiagnosticCodeName(code: number) {
    return ({
        1: 'BLE stack initialized', 2: 'BLE stack initialization failed',
        3: 'BLE role started', 4: 'BLE role start failed', 5: 'BLE stopped',
        6: 'Central scan started', 7: 'Central scan start failed',
        8: 'Central scan stop failed', 9: 'Peer discovered',
        10: 'Central peer/client slot allocation failed', 11: 'Central connect attempt',
        12: 'Connection failure', 13: 'Central GAP connected',
        14: 'Central GATT handshake step', 15: 'Central handshake complete',
        16: 'Peripheral accepted GAP connection', 17: 'Peripheral rejected incoming GAP connection',
        18: 'Peripheral link slot allocation failed', 19: 'Peripheral accepted peer identity',
        20: 'Peripheral rejected peer identity', 21: 'Peripheral handshake timeout',
        22: 'Peer disconnected', 23: 'Advertising restart failed',
        24: 'Central scan restart failed', 25: 'GAP connection callback',
        26: 'BLE event queue full', 27: 'BLE receive queue full',
        28: 'NimBLE connection capacity', 29: 'GAP lifecycle event',
        30: 'Peripheral onConnect callback entered',
        31: 'Peripheral connection admission decision',
        32: 'Peripheral peer identity write',
        33: 'Peripheral peer identity decision',
        34: 'Central connect() return value',
        35: 'GAP connection handle lookup',
        36: 'Peripheral disconnect callback',
        37: 'Raw GAP connection handle',
        38: 'Central configured connection timeout',
        39: 'Peripheral advertising configuration',
        40: 'BLE address detail'
    } as Record<number, string>)[code] ?? `BLE diagnostic ${code}`;
}
function bleDiagnosticStageText(record: BleDiagnosticRecord) {
    if (record.code === 2 && record.stage) {
        return ({
            1: 'NimBLE init', 2: 'Set ATT MTU', 3: 'Register GAP callback',
            4: 'Create GATT server', 5: 'Create GATT service',
            6: 'Create GATT characteristics', 7: 'Start GATT server',
            8: 'Get Central scanner'
        } as Record<number, string>)[record.stage] ?? `init step ${record.stage}`;
    }
    if (record.code === 4 && record.stage) {
        const subsystem = record.reason === 2 ? 'advertising' : record.reason === 3 ? 'Central scan' : 'BLE role';
        const step = record.stage === 1 ? 'object unavailable' : 'start call failed';
        return `${subsystem}: ${step}`;
    }
    if (record.code === 38) return 'configured Central GAP timeout';
    if (record.code === 39) {
        return ({ 1: 'connectable advertising mode', 2: 'connection whitelist filter', 3: 'advertising interval' } as Record<number, string>)[record.stage] ?? `advertising setting ${record.stage}`;
    }
    if (record.code === 40) {
        return ({ 0: 'address type', 1: 'address bytes 0–1', 2: 'address bytes 2–3', 3: 'address bytes 4–5' } as Record<number, string>)[record.stage] ?? `address part ${record.stage}`;
    }
    if ([8, 10, 12, 14, 20, 21, 25].includes(record.code) && record.stage) {
        return bleConnectionStageName(record.stage);
    }
    if (record.code === 28 && record.stage) {
        return ({
            1: 'NimBLE C++ client pool',
            2: 'NimBLE host alias',
            3: 'ESP-IDF legacy setting',
            4: 'Effective Mynewt host setting'
        } as Record<number, string>)[record.stage] ?? `capacity source ${record.stage}`;
    }
    if (record.code === 29) {
        return ({
            0: 'CONNECT', 1: 'DISCONNECT', 3: 'CONN_UPDATE',
            6: 'TERM_FAILURE', 8: 'DISC_COMPLETE', 9: 'ADV_COMPLETE',
            10: 'ENC_CHANGE', 14: 'SUBSCRIBE', 15: 'MTU',
            38: 'LINK_ESTAB'
        } as Record<number, string>)[record.reason] ?? `GAP event ${record.reason}`;
    }
    if (record.code === 35) {
        return record.reason === 0 ? 'after CONNECT' :
            record.reason === 38 ? 'after LINK_ESTAB' : `after GAP event ${record.reason}`;
    }
    if (record.code === 37) {
        return ({
            0: 'CONNECT', 1: 'DISCONNECT', 38: 'LINK_ESTAB'
        } as Record<number, string>)[record.reason] ?? `GAP event ${record.reason}`;
    }
    if (record.code === 32) {
        return ({ 0: 'write accepted', 1: 'invalid payload length', 2: 'no matching connection handle' } as Record<number, string>)[record.reason] ?? `write result ${record.reason}`;
    }
    return record.stage ? `stage ${record.stage}` : '';
}
function bleDiagnosticLine(record: BleDiagnosticRecord) {
    const mode = (record.stateFlags >> 3) & 0x03;
    const state = [
        bleModeName(mode),
        record.stateFlags & 1 ? 'enabled' : 'disabled',
        record.stateFlags & 2 ? 'advertising' : '',
        record.stateFlags & 4 ? 'scanning' : ''
    ].filter(Boolean).join(', ');
    const detail = record.code === 38
        ? `${record.detail} ms`
        : record.code === 39 && record.stage === 1
            ? (record.detail === 0 ? 'set successfully' : 'set failed')
            : record.code === 39 && record.stage === 2
                ? 'disabled'
                : record.code === 39 && record.stage === 3
                    ? `${record.detail} units (${(record.detail * 0.625).toFixed(1)} ms)`
                    : record.code === 40 && record.stage === 0
                        ? (record.detail === 0 ? 'Public (0)' : record.detail === 1 ? 'Random (1)' : `type ${record.detail}`)
                        : record.code === 40
                            ? (() => {
                                const value = record.detail & 0xffff;
                                const low = (value & 0xff).toString(16).toUpperCase().padStart(2, '0');
                                const high = ((value >> 8) & 0xff).toString(16).toUpperCase().padStart(2, '0');
                                return `${low}:${high}`;
                            })()
                            : [3, 4].includes(record.stage)
                                ? bleConnectionDetailText(record.stage, record.detail)
                                : record.detail < 0 ? String(record.detail)
                                    : `0x${(record.detail & 0xffff).toString(16).toUpperCase().padStart(4, '0')} (${record.detail})`;
    const connectionReason = ({
        1: 'local setup', 3: 'identity rejected',
        5: 'GATT/handshake failure', 6: 'GAP failure'
    } as Record<number, string>)[record.reason];
    const peripheralRejectReason = ({
        1: 'BLE disabled', 2: 'wrong BLE role', 3: 'eight-link limit reached'
    } as Record<number, string>)[record.reason];
    const startFailureSubsystem = ({
        1: 'invalid or idle mode', 2: 'advertising', 3: 'Central scan'
    } as Record<number, string>)[record.reason];
    const reason = record.code === 12 || record.code === 25 ? (connectionReason ?? '') :
        record.code === 17 ? (peripheralRejectReason ?? '') :
            record.code === 4 ? (startFailureSubsystem ?? '') :
                record.code === 26 ? `dropped event 0x${record.reason.toString(16).toUpperCase().padStart(2, '0')}` :
                    record.code === 28 ? `application link limit ${record.reason}` :
                        record.code === 29 ? '' :
                            record.code === 30 ? `connection handle ${record.detail}` :
                                record.code === 31 ? ({
                                    0: record.stage === 1 ? 'application accepted incoming connection' : 'link slot allocated',
                                    1: 'rejected: BLE disabled', 2: 'rejected: not in Peripheral mode',
                                    3: 'rejected: application link limit reached',
                                    4: 'rejected: no free application link slot'
                                } as Record<number, string>)[record.reason] ?? `admission reason ${record.reason}` :
                                    record.code === 32 ? '' :
                                        record.code === 33 ? ({
                                            0: 'peer identity accepted', 1: 'rejected: BLE disabled',
                                            2: 'rejected: not a Peripheral link', 3: 'rejected: handshake timed out',
                                            4: 'rejected: peer is not allow-listed', 5: 'rejected: duplicate peer identity'
                                        } as Record<number, string>)[record.reason] ?? `identity decision ${record.reason}` :
                                            record.code === 34 ? (record.reason ? 'connect() started' : 'connect() rejected synchronously') :
                                                record.code === 35 ? (record.detail === 0 ? 'connection handle lookup succeeded' :
                                                    `connection handle lookup failed: ${record.detail}`) :
                                                    record.code === 36 ? `disconnect reason ${record.reason}, handle ${record.detail < 0 ? 'none' : record.detail}` :
                                                        record.code === 37 ? `handle ${record.detail < 0 ? 'none' : record.detail}` :
                                                            record.code === 39 && record.stage === 1 ? 'undirected connectable' :
                                                                record.code === 40 ? ({
                                                                    0: 'local device address', 1: 'advertiser address observed by scan',
                                                                    2: 'address used by Central connect()'
                                                                } as Record<number, string>)[record.reason] ?? `address source ${record.reason}` :
                                                                    record.reason ? `reason ${record.reason}` : '';
    const capacityDetail = record.code === 28
        ? record.detail < 0 ? 'capacity macro unavailable' :
            record.stage === 1 ? `NimBLE C++ client pool ${record.detail}` :
                record.stage === 2 ? `NimBLE host alias ${record.detail}` :
                    record.stage === 3 ? `ESP-IDF legacy setting ${record.detail}` :
                        record.stage === 4 ? `effective host slots ${record.detail}` : `capacity ${record.detail}`
        : '';
    return [
        `${(record.uptimeMs / 1000).toFixed(1)}s`,
        bleDiagnosticCodeName(record.code),
        record.peerId ? `peer ${record.peerId}` : 'peer unknown',
        bleDiagnosticStageText(record),
        reason,
        capacityDetail || (record.detail || [25, 39, 40].includes(record.code) ? detail : ''),
        `${record.activeLinks}/8 links`,
        state
    ].filter(Boolean).join(' · ');
}
async function refreshDiagnostics() {
    await refreshAll();
    await refreshTrace();
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
        // Message fetches have a short firmware consumption deadline. Run
        // them ahead of queued UI refreshes and other not-yet-started commands.
        const p = await cmd(OP.GET_RECEIVED_DATA, [...u16(peerId), ...u16(messageId)], true);
        if (p.length < 4) throw new Error(`GET_RECEIVED_DATA returned only ${p.length} bytes.`);
        const returnedId = get16(p, 0), length = get16(p, 2);
        if (returnedId !== messageId || length !== p.length - 4 || length !== announcedLength) {
            throw new Error(`Received BLE message length/ID mismatch (event ${messageId}/${announcedLength}, response ${returnedId}/${length}).`);
        }
        const bytes = p.slice(4);
        ble.inbox.unshift({
            peerId, messageId, length, time: Date.now(), state: 'Retrieved',
            text: new TextDecoder('utf-8').decode(new Uint8Array(bytes)), hex: formatHexBytes(bytes)
        });
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
    traceRecords.value = [];
    traceFetchedAt.value = '';
    traceError.value = '';
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
        await cmd(OP.SET_ROLE, [calibration.originalRole]);
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
        traceRecords.value = [];
        traceFetchedAt.value = '';
        traceError.value = '';
        lastTraceRefreshAt = 0;
        log(`USB connected · ${selectedPort.value}`);
        await cmd(OP.PING);
        await refreshAll();
        await cmd(OP.SUBSCRIBE, u16(0x7fff));

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

type ConfigPresetData = {
    deviceId: number; panId: string; channel: number; tx: number; rx: number;
    period: number; timeout: number; retries: number; role: number;
    targets: string; autoStart: boolean; bleConfig: BleConfig; blePeers: string;
};
type ConfigPreset = { id: string; name: string; updatedAt: string; config: ConfigPresetData };
const CONFIG_PRESETS_KEY = 'd3km.configuration-presets.v1';
const CONFIG_PRESET_FORMAT = 'd3km-configuration-preset';
const CONFIG_PRESET_VERSION = 1;
const configPresets = ref<ConfigPreset[]>([]);
const selectedConfigPresetId = ref('');
const presetName = ref('');
const presetImportInput = ref<HTMLInputElement | null>(null);

function clonePresetData(): ConfigPresetData {
    return {
        deviceId: Number(draft.deviceId), panId: String(draft.panId).toUpperCase(),
        channel: Number(draft.channel), tx: Number(draft.tx), rx: Number(draft.rx),
        period: Number(draft.period), timeout: Number(draft.timeout), retries: Number(draft.retries),
        role: Number(draft.role), targets: String(draft.targets), autoStart: Boolean(draft.autoStart),
        bleConfig: cloneBleConfig(bleDraft), blePeers: String(blePeersInput.value)
    };
}
function loadConfigPresets() {
    try {
        const raw = localStorage.getItem(CONFIG_PRESETS_KEY);
        const parsed = raw ? JSON.parse(raw) : [];
        configPresets.value = Array.isArray(parsed) ? parsed.filter((item: any) =>
            item && typeof item.id === 'string' && typeof item.name === 'string' &&
            item.config && typeof item.config === 'object'
        ) : [];
    } catch {
        configPresets.value = [];
        error.value = 'Saved configuration presets could not be read from this browser.';
    }
}
function persistConfigPresets() {
    try {
        localStorage.setItem(CONFIG_PRESETS_KEY, JSON.stringify(configPresets.value));
        return true;
    } catch (e: any) {
        error.value = `Could not persist configuration presets in this browser: ${String(e)}`;
        return false;
    }
}
function saveConfigPreset() {
    const name = presetName.value.trim();
    if (!name) { error.value = 'Enter a name for this configuration preset.'; return; }
    if (name.length > 80) { error.value = 'Preset names must be 80 characters or fewer.'; return; }
    const existing = configPresets.value.find(item => item.id === selectedConfigPresetId.value);
    const now = new Date().toISOString();
    const preset: ConfigPreset = {
        id: existing?.id ?? (typeof crypto !== 'undefined' && 'randomUUID' in crypto ? crypto.randomUUID() : `preset-${Date.now()}-${Math.random().toString(36).slice(2)}`),
        name, updatedAt: now, config: clonePresetData()
    };
    const previous = configPresets.value;
    configPresets.value = existing
        ? configPresets.value.map(item => item.id === existing.id ? preset : item)
        : [...configPresets.value, preset];
    if (!persistConfigPresets()) { configPresets.value = previous; return; }
    selectedConfigPresetId.value = preset.id;
    presetName.value = preset.name;
    notice.value = existing ? `Updated preset “${name}”. Device NVS was not changed.` : `Saved preset “${name}”. Device NVS was not changed.`;
}
function applyConfigPreset(id = selectedConfigPresetId.value) {
    const preset = configPresets.value.find(item => item.id === id);
    if (!preset) return;
    const c = preset.config;
    Object.assign(draft, {
        deviceId: c.deviceId, panId: c.panId, channel: c.channel, tx: c.tx, rx: c.rx,
        period: c.period, timeout: c.timeout, retries: c.retries, role: c.role,
        targets: c.targets, autoStart: c.autoStart
    });
    Object.assign(bleDraft, cloneBleConfig(c.bleConfig));
    blePeersInput.value = c.blePeers;
    selectedConfigPresetId.value = preset.id;
    presetName.value = preset.name;
    notice.value = `Loaded preset “${preset.name}” into the editor. Save configuration to apply it to the board and NVS.`;
    error.value = '';
}
function deleteConfigPreset() {
    const preset = configPresets.value.find(item => item.id === selectedConfigPresetId.value);
    if (!preset) return;
    const previous = configPresets.value;
    configPresets.value = configPresets.value.filter(item => item.id !== preset.id);
    if (!persistConfigPresets()) { configPresets.value = previous; return; }
    selectedConfigPresetId.value = '';
    presetName.value = '';
    notice.value = `Deleted preset “${preset.name}”.`;
}
function exportConfigPreset() {
    const preset = configPresets.value.find(item => item.id === selectedConfigPresetId.value);
    if (!preset) { error.value = 'Select a saved preset to export.'; return; }
    const payload = { format: CONFIG_PRESET_FORMAT, version: CONFIG_PRESET_VERSION, ...preset };
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `d3km-config-${preset.name.trim().toLowerCase().replace(/[^a-z0-9]+/g, '-') || 'preset'}.json`;
    anchor.click();
    URL.revokeObjectURL(url);
    notice.value = `Exported “${preset.name}”. Share the JSON file with others.`;
}
function validateImportedPreset(value: any): ConfigPreset {
    if (!value || value.format !== CONFIG_PRESET_FORMAT || value.version !== CONFIG_PRESET_VERSION ||
        typeof value.name !== 'string' || !value.name.trim() || value.name.length > 80 ||
        !value.config || typeof value.config !== 'object') {
        throw new Error('This is not a supported D3KM configuration preset file.');
    }
    const c = value.config;
    const n = (key: string) => Number(c[key]);
    const targets = typeof c.targets === 'string' ? c.targets : '';
    const peers = c.blePeers == null ? '' : String(c.blePeers);
    if (!Number.isInteger(n('deviceId')) || n('deviceId') < 1 || n('deviceId') > 65534 ||
        !/^[0-9A-Fa-f]{1,4}$/.test(String(c.panId)) ||
        ![5, 9].includes(n('channel')) ||
        ![0, 1, 2].includes(n('role')) ||
        !Number.isInteger(n('tx')) || n('tx') < 0 || n('tx') > 65535 ||
        !Number.isInteger(n('rx')) || n('rx') < 0 || n('rx') > 65535 ||
        !Number.isInteger(n('period')) || n('period') < 20 || n('period') > 60000 ||
        !Number.isInteger(n('timeout')) || n('timeout') < 50 || n('timeout') > 5000 ||
        !Number.isInteger(n('retries')) || n('retries') < 0 || n('retries') > 5 ||
        typeof c.autoStart !== 'boolean') {
        throw new Error('The imported preset contains invalid UWB settings.');
    }
    const targetIds = targets.trim() ? targets.split(',').map((item: string) => Number(item.trim())) : [];
    if (targetIds.length > 16 || targetIds.some((id: number) => !Number.isInteger(id) || id < 1 || id > 65534) ||
        targetIds.includes(n('deviceId')) || new Set(targetIds).size !== targetIds.length) {
        throw new Error('The imported preset contains invalid or duplicate target IDs.');
    }
    const bleConfig = c.bleConfig as BleConfig;
    if (!bleConfig || !Array.isArray(bleConfig.peers)) throw new Error('The imported preset is missing its Bluetooth configuration.');
    validateBleConfig(bleConfig);
    const parsedPeers = peers.trim() ? parseBlePeerIds(peers) : [];
    if (JSON.stringify(parsedPeers) !== JSON.stringify(bleConfig.peers)) {
        throw new Error('The imported Bluetooth peer list does not match its settings.');
    }
    return {
        id: typeof value.id === 'string' ? value.id : `import-${Date.now()}-${Math.random().toString(36).slice(2)}`,
        name: value.name.trim(), updatedAt: typeof value.updatedAt === 'string' ? value.updatedAt : new Date().toISOString(),
        config: {
            deviceId: n('deviceId'), panId: String(c.panId).toUpperCase().padStart(4, '0'),
            channel: n('channel'), tx: n('tx'), rx: n('rx'), period: n('period'),
            timeout: n('timeout'), retries: n('retries'), role: n('role'),
            targets, autoStart: c.autoStart, bleConfig: cloneBleConfig(bleConfig), blePeers: parsedPeers.join(', ')
        }
    };
}
async function importConfigPreset(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;
    try {
        const preset = validateImportedPreset(JSON.parse(await file.text()));
        const duplicateName = configPresets.value.some(item => item.name.toLocaleLowerCase() === preset.name.toLocaleLowerCase());
        const imported = duplicateName ? { ...preset, id: `import-${Date.now()}-${Math.random().toString(36).slice(2)}`, name: `${preset.name} (imported)` } : preset;
        const previous = configPresets.value;
        configPresets.value = [...previous, imported];
        if (!persistConfigPresets()) { configPresets.value = previous; return; }
        selectedConfigPresetId.value = imported.id;
        presetName.value = imported.name;
        applyConfigPreset(imported.id);
        notice.value = `Imported preset “${imported.name}”. It is loaded in the editor; the board has not been changed.`;
    } catch (e: any) {
        error.value = `Could not import configuration preset: ${String(e)}`;
    } finally {
        input.value = '';
    }
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
    const p = await cmd(OP.GET_STATUS);
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
            cmd(OP.GET_INFO), cmd(OP.GET_STATUS), cmd(OP.GET_CONFIG), cmd(OP.GET_SAVED_CONFIG), cmd(OP.GET_STATISTICS), cmd(OP.GET_TARGETS), cmd(OP.GET_UWB_SCHEDULE), cmd(OP.GET_DIAGNOSTICS), cmd(OP.GET_LAST_ERROR)
        ]);
        if (i.length !== 21) throw new Error(`GET_INFO returned ${i.length} bytes; expected 21.`);
        const saved = decodeSavedConfig(savedPayload);
        applyBleStatusPayload(s);
        const runtimeBleConfig = decodeBleConfigExtension(runtime, 16);
        ble.supported = i[12] === 1 && s.length >= 27 && runtime.length >= 32 && saved.bleConfigAvailable;
        Object.assign(ble.runtimeConfig, cloneBleConfig(runtimeBleConfig));
        Object.assign(ble.savedConfig, cloneBleConfig(saved.bleConfig));
        Object.assign(bleDraft, cloneBleConfig(saved.bleConfig));
        blePeersInput.value = bleDraft.peers.join(', ');
        ble.peerStatuses = [];
        ble.discoveredPeers = [];
        device.info = {
            version: `${i[0]}.${i[1]}.${i[2]}`, protocol: i[3], id: get16(i, 4),
            maxTargets: i[6], history: get16(i, 7), radio: i[9], maxPayload: get16(i, 10),
            bleSupported: i[12] === 1, bleModeMask: i[13], maxBlePeers: i[14],
            maxBleConnections: i[15], maxBleMessageLength: get16(i, 16),
            maxBleMtu: get16(i, 18), bleProtocolVersion: i[20]
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
        if (st.length !== 68) throw new Error(`GET_STATISTICS returned ${st.length} bytes; expected 68.`);
        device.stats = {
            ok: get32(st), failed: get32(st, 4), rx: get32(st, 8), tx: get32(st, 12),
            timeouts: get32(st, 16), protocol: get32(st, 20), overflows: get32(st, 24),
            overwrites: get32(st, 28), malformed: get32(st, 32), duplicates: get32(st, 36),
            bleMessagesSent: get32(st, 40), bleMessagesReceived: get32(st, 44),
            bleTxErrors: get32(st, 48), bleRxErrors: get32(st, 52),
            bleConnectionFailures: get32(st, 56), bleRxQueueOverflows: get32(st, 60),
            bleEventQueueOverflows: get32(st, 64)
        };
        // These commands intentionally describe current RAM state for the Ranging test page.
        device.targets = Array.from({ length: t[0] }, (_, j) => get16(t, 1 + j * 2));
        device.schedule = {
            period: get32(sc), timeout: get16(sc, 4), retries: sc[6], targetCount: sc[7],
            queued: sc[8], active: sc[9] === 1, effective: get32(sc, 10), average: get32(sc, 14)
        };
        if (d.length !== 29) throw new Error(`GET_DIAGNOSTICS returned ${d.length} bytes; expected 29.`);
        device.diagnostics = {
            radio: get32(d), live: get32(d, 4), malformed: get32(d, 8), rx: get32(d, 12),
            tx: get32(d, 16), state: d[20], standby: d[21] === 1,
            bleMode: d[22], bleEnabled: d[23] === 1, bleAdvertising: d[24] === 1,
            bleScanning: d[25] === 1, bleActiveLinks: d[26],
            bleRxQueueDepth: d[27], bleEventQueueDepth: d[28]
        };
        if (le.length !== 16) throw new Error(`GET_LAST_ERROR returned ${le.length} bytes; expected 16.`);
        device.lastError = {
            subsystem: le[0], code: le[1], time: get64(le, 2).toString(),
            peerId: get16(le, 10), context: get32(le, 12)
        };
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
        if (!ble.supported) throw new Error('This firmware did not report the required BLE capability and complete configuration.');
        bleDraft.peers = parseBlePeerIds(blePeersInput.value);
        validateBleConfig(bleDraft);
        savingConfig.value = true;

        // One explicit snapshot, including the BLE extension, is applied to runtime RAM and NVS.
        // It cannot accidentally persist temporary settings changed by Ranging test.
        const payload = [
            ...u16(draft.deviceId), ...u16(pan), draft.channel, ...u16(draft.tx), ...u16(draft.rx),
            ...u32(draft.period), ...u16(draft.timeout), draft.retries, draft.role,
            targets.length, ...targets.flatMap(u16), draft.autoStart ? 1 : 0,
            ...encodeBleConfigExtension(bleDraft)
        ];
        savingStep.value = 'Applying configuration and committing this exact snapshot to NVS…';
        try {
            await cmd(OP.APPLY_SAVE_CONFIG, payload);
        } catch (applyError: any) {
            // If the ACK was lost after commit, require both persistent and runtime
            // readback to match. NVS alone is insufficient if the command was BUSY.
            const [savedPayload, runtime, status, runtimeTargetsPayload] = await Promise.all([
                cmd(OP.GET_SAVED_CONFIG), cmd(OP.GET_CONFIG), cmd(OP.GET_STATUS), cmd(OP.GET_TARGETS)
            ]);
            const readback = decodeSavedConfig(savedPayload);
            const runtimeTargets = Array.from({ length: runtimeTargetsPayload[0] }, (_, i) => get16(runtimeTargetsPayload, 1 + i * 2));
            const savedMatches = readback.deviceId === draft.deviceId && readback.panId === pan &&
                readback.channel === draft.channel && readback.tx === draft.tx && readback.rx === draft.rx &&
                readback.period === draft.period && readback.timeout === draft.timeout &&
                readback.retries === draft.retries && readback.role === draft.role &&
                readback.autoStart === draft.autoStart && JSON.stringify(readback.targets) === JSON.stringify(targets) &&
                (readback.bleConfigAvailable && JSON.stringify(readback.bleConfig) === JSON.stringify(bleDraft));
            const runtimeBleMatches = runtime.length >= 32 &&
                JSON.stringify(decodeBleConfigExtension(runtime, 16)) === JSON.stringify(bleDraft);
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
            await cmd(OP.GET_STATUS);
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
    await cmd(OP.SET_ANTENNA_DELAYS, [...u16(tx), ...u16(rx)]);
    const readback = await cmd(OP.GET_ANTENNA_DELAYS);
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
                response = await cmd(OP.RANGE_ONCE, u16(Number(calibration.referenceId)));
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
                try { await cmd(OP.CANCEL_OPERATION, u16(jobId)); } catch { /* It may already have completed. */ }
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
        const status = await cmd(OP.GET_STATUS);
        if (status[1] === 1) throw new Error('Stop continuous ranging before starting antenna calibration.');
        if (status[2] !== 1 || status[3] === 1) throw new Error('The connected radio must be ready and awake.');
        const schedule = await cmd(OP.GET_UWB_SCHEDULE);
        if (schedule[8] !== 0 || schedule[9] !== 0) throw new Error('Wait for active and queued range transactions to finish.');
        const subscription = await cmd(OP.SUBSCRIBE, u16(0x0003));
        if (subscription.length < 2 || (get16(subscription, 0) & 0x0003) !== 0x0003) {
            throw new Error('Could not subscribe to range result and failure events on this USB connection.');
        }
        const [antenna, savedPayload] = await Promise.all([cmd(OP.GET_ANTENNA_DELAYS), cmd(OP.GET_SAVED_CONFIG)]);
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
        if (status[0] !== 1) { await cmd(OP.SET_ROLE, [1]); await refreshStatus(); }
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
        const savedPair = await cmd(OP.SAVE_ANTENNA_DELAYS);
        if (savedPair.length !== 4 || get16(savedPair, 0) !== calibration.requestedDelay || get16(savedPair, 2) !== calibration.requestedDelay) {
            throw new Error('The firmware did not confirm the requested antenna delays were saved to NVS.');
        }
        const verify = decodeSavedConfig(await cmd(OP.GET_SAVED_CONFIG));
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
        const saved = decodeSavedConfig(await cmd(OP.GET_SAVED_CONFIG));
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
        await cmd(OP.RESET_CONFIG);
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
        await cmd(OP.SET_ROLE, [role]);
        await refreshStatus();
        notice.value = `Runtime role changed to ${['Idle', 'Initiator', 'Responder'][role]}. NVS configuration was not changed.`;
        log(`Temporary RAM role set · ${roleName.value}`);
    } catch (e: any) {
        error.value = String(e);
    }
}
async function startRange() { try { await cmd(OP.START_RANGING); await refreshStatus(); notice.value = 'Ranging started'; } catch (e: any) { error.value = String(e) } }
async function stopRange() { try { await cmd(OP.STOP_RANGING); await refreshStatus(); notice.value = 'Ranging stopped and queued one-shots cleared'; } catch (e: any) { error.value = String(e) } }
async function rangeOnce() { try { const target = Number(oneShotTarget.value); if (!Number.isInteger(target) || target < 1 || target > 65534) throw new Error('Enter a valid target ID.'); const p = await cmd(OP.RANGE_ONCE, u16(target)); notice.value = `One-shot job ${get16(p)} queued for target ${target}`; log(`RANGE_ONCE queued · target ${target} · job ${get16(p)}`) } catch (e: any) { error.value = String(e) } }
const oneShotTarget = ref('2'), cancelJobId = ref('1'), advancedOutput = ref('No advanced command run yet.');
async function loadHistory() { try { let cursor = 0, rows: any[] = []; for (let page = 0; page < 16; page++) { const p = await cmd(OP.GET_HISTORY, [...u32(cursor), ...u16(17), ...u16(0xffff)]); const count = p[13]; cursor = get32(p, 8); for (let j = 0; j < count; j++)rows.push(decodeMeasurement(p.slice(14 + j * 28, 14 + (j + 1) * 28))); if (count < 17 || cursor === 0) break; } measurements.value = rows.reverse().slice(-256); notice.value = `Loaded ${measurements.value.length} retained measurements`; } catch (e: any) { error.value = String(e) } }
async function quick(op: number, label: string) { try { const p = await cmd(op); notice.value = label; log(label); return p } catch (e: any) { error.value = String(e); return null } }
async function pingNode() { const p = await quick(1, 'PING completed'); if (p) advancedOutput.value = `PING response: ${readString(p)}` }
async function syncClock() { try { const us = BigInt(Date.now()) * 1000n; const p = await cmd(OP.TIME_SYNC, Array.from({ length: 8 }, (_, i) => Number((us >> BigInt(8 * i)) & 255n))); advancedOutput.value = `TIME_SYNC · ESP monotonic: ${get64(p).toString()} µs · epoch offset: ${BigInt.asIntN(64, get64(p, 8)).toString()} µs`; notice.value = 'Device clock synchronized'; } catch (e: any) { error.value = String(e) } }
async function readLastRange() { try { const p = await cmd(OP.GET_LAST_RANGE, [0xff, 0xff]); const count = p[0]; const rows = Array.from({ length: count }, (_, i) => decodeMeasurement(p.slice(1 + i * 28, 1 + (i + 1) * 28))); measurements.value = rows; advancedOutput.value = `GET_LAST_RANGE returned ${count} newest retained measurement(s), one per target.`; navigate('history'); } catch (e: any) { error.value = String(e) } }
async function cancelJob() { try { await cmd(OP.CANCEL_OPERATION, u16(Number(cancelJobId.value))); advancedOutput.value = `Cancel request sent for job ${cancelJobId.value}.`; notice.value = 'Operation cancellation acknowledged'; } catch (e: any) { error.value = String(e) } }
async function subscribeAll() { try { const p = await cmd(OP.SUBSCRIBE, u16(0x7fff)); advancedOutput.value = `Active event subscription mask: ${hex(get16(p))}`; notice.value = 'Subscribed to all UWB and BLE event types'; } catch (e: any) { error.value = String(e) } }
async function unsubscribeAll() { try { const p = await cmd(OP.UNSUBSCRIBE, u16(0x7fff)); advancedOutput.value = `Active event subscription mask: ${hex(get16(p))}`; notice.value = 'Unsubscribed from all UWB and BLE event types'; } catch (e: any) { error.value = String(e) } }
async function readAntenna() { try { const p = await cmd(OP.GET_ANTENNA_DELAYS); advancedOutput.value = `TX antenna delay: ${get16(p)} · RX antenna delay: ${get16(p, 2)}`; } catch (e: any) { error.value = String(e) } }
async function clearStats() { if (confirm('Clear all device diagnostic counters?')) { await quick(5, 'Statistics counters cleared'); await refreshAll() } }
async function clearHistory() { if (confirm('Clear retained measurement history?')) { await quick(0x34, 'Measurement history cleared'); measurements.value = [] } }
async function sleepRadio() { await quick(0x40, 'Radio entered IDLE_RC standby (not deep sleep)'); await refreshStatus() }
async function wakeRadio() { await quick(0x41, 'Radio wake command completed'); await refreshStatus() }
async function reboot() { if (confirm('Reboot the ESP32 node? The USB connection will be interrupted.')) { await quick(0x43, 'Reboot command sent'); setTimeout(() => { void handleUsbLoss('Device rebooted. Reconnect when it appears on USB.'); }, 1000); } }
async function systemSleep() { const sec = Number(prompt('Timer wake duration in seconds (1–604800):', '60')); if (!Number.isInteger(sec) || sec < 1 || sec > 604800) return; try { await cmd(OP.SYSTEM_SLEEP, u32(sec * 1000)); await handleUsbLoss('Device entered deep sleep. Reconnect after timer wake.'); } catch (e: any) { error.value = String(e) } }
function fmtDuration(ms: number) { const s = Math.floor(ms / 1000); return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m ${s % 60}s` }
function statusText(s: number) { return ['Valid', 'Timeout', 'Invalid', 'Cancelled', 'Radio error'][s] ?? 'Unknown' }
function fmtRange(mm: number | null) { return mm === null || mm < 0 ? '—' : `${(mm / 1000).toFixed(3)} m` }
watch(connected, (isConnected) => {
    if (!isConnected) active.value = 'overview';
});
async function drainDeviceEvents() {
    // The Tauri serial reader only runs while a command is waiting for its
    // response. Keep a lightweight GET_STATUS read active while BLE is enabled
    // so unsolicited BLE_DATA_RECEIVED events reach the UI well before the
    // firmware's receive-consumption deadline. Any command already in flight
    // is itself reading the port, so do not queue a redundant poll behind it.
    if (!connected.value || !ble.supported || !ble.runtime.enabled || eventDrainBusy ||
        polling || commandInFlight || priorityCommandQueue.length || normalCommandQueue.length) return;
    eventDrainBusy = true;
    try {
        await cmd(OP.GET_STATUS);
    } catch (e: any) {
        if (isUsbDisconnectError(e)) void handleUsbLoss('USB connection lost. Reconnect the device to continue.');
    } finally {
        eventDrainBusy = false;
    }
}

onMounted(async () => {
    loadConfigPresets();
    await refreshPorts();
    unlisten = await listen<FrameEvent>('uwb-event', e => handleEvent(e.payload));
    eventDrainTimer = window.setInterval(() => { void drainDeviceEvents(); }, 50);
    poller = window.setInterval(async () => {
        await refreshPorts();
        if (connected.value && !polling && !savingConfig.value && !calibration.active && !calibration.processing && !calibration.activeJobId) {
            polling = true;
            try {
                await refreshStatus();
                if (active.value === 'diagnostics' && Date.now() - lastTraceRefreshAt >= 1200) {
                    lastTraceRefreshAt = Date.now();
                    await refreshTrace(true);
                }
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
    if (eventDrainTimer) clearInterval(eventDrainTimer);
    unlisten?.();
});
