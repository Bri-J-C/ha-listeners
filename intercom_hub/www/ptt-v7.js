/**
 * Intercom PTT Web Client
 *
 * Connects to the Intercom Hub add-on via WebSocket and provides
 * push-to-talk functionality from any web browser.
 *
 * Mic is acquired on init, released when app is backgrounded,
 * and re-acquired when app returns to foreground.
 *
 * Priority levels (must match firmware protocol.h and hub):
 *   0 = Normal  — first-to-talk collision avoidance
 *   1 = High    — overrides Normal transmissions
 *   2 = Emergency — overrides all, bypasses DND on receiver
 */

// Priority constants (kept in sync with firmware protocol.h)
const PRIORITY_NORMAL = 0;
const PRIORITY_HIGH = 1;
const PRIORITY_EMERGENCY = 2;

class IntercomPTT {
    constructor() {
        // State
        this.audioContext = null;
        this.mediaStream = null;
        this.workletNode = null;
        this.mediaSource = null;
        this.websocket = null;
        this.isConnected = false;
        this.isTransmitting = false;
        this.isReceiving = false;
        this.lastCallTime = 0;  // Timestamp of last call sent (ms), for PTT lockout
        this.isInitialized = false;
        this.micEnabled = false;
        this.pendingCaller = null;
        this.clientId = null;  // This client's identity (e.g., "Brians_Phone")

        // Priority and DND state
        this.priority = PRIORITY_NORMAL;  // Our TX priority
        this.dndEnabled = false;          // Client-side DND: only play EMERGENCY audio

        // Parse caller and device identity from URL
        const params = new URLSearchParams(window.location.search);
        const callerParam = params.get('caller');
        const deviceParam = params.get('device');

        if (callerParam) {
            this.pendingCaller = decodeURIComponent(callerParam).replace(/_/g, ' ');
            console.log('Caller from URL:', this.pendingCaller);
        }
        if (deviceParam) {
            // Mobile device from notification - use device name
            this.clientId = decodeURIComponent(deviceParam).replace(/_/g, ' ');
            console.log('Client identity from URL:', this.clientId);
        } else {
            // Regular web client - get/generate persistent ID
            this.clientId = this.getOrCreateClientId();
            console.log('Client identity from storage:', this.clientId);
        }

        // WebSocket reconnect backoff
        this.reconnectDelay = 1000;
        this.maxReconnectDelay = 30000;
        this.reconnectTimer = null;

        // Audio playback
        this.nextPlayTime = 0;
        this.playbackBuffer = null;  // Reusable Float32Array for playAudio

        // UI Elements
        this.initOverlay = document.getElementById('initOverlay');
        this.initButton = document.getElementById('initButton');
        this.pttButton = document.getElementById('pttButton');
        this.connStatus = document.getElementById('connStatus');
        this.pttStatus = document.getElementById('pttStatus');
        this.targetSelect = document.getElementById('targetSelect');
        this.callButton = document.getElementById('callButton');
        this.errorBanner = document.getElementById('errorBanner');
        this.deviceNameInput = document.getElementById('deviceNameInput');
        this.prioritySelect = document.getElementById('prioritySelect');
        this.dndToggle = document.getElementById('dndToggle');

        // Pre-fill device name input from localStorage
        const savedName = localStorage.getItem('intercom_custom_name');
        if (savedName) {
            this.deviceNameInput.value = savedName;
        }

        // Bind event handlers
        this.initButton.addEventListener('click', () => this.initialize());
        this.callButton.addEventListener('click', () => this.sendCall());

        // PTT button - use pointer events for unified touch/mouse handling
        this.pttButton.addEventListener('pointerdown', (e) => {
            e.preventDefault();
            this.pttButton.setPointerCapture(e.pointerId);
            this.startTransmit(e);
        });
        this.pttButton.addEventListener('pointerup', (e) => {
            e.preventDefault();
            this.stopTransmit(e);
        });
        this.pttButton.addEventListener('pointercancel', (e) => {
            e.preventDefault();
            this.stopTransmit(e);
        });
        this.pttButton.addEventListener('pointerleave', (e) => {
            // Only stop if we don't have capture (prevents stopping when finger moves)
            if (!this.pttButton.hasPointerCapture(e.pointerId)) {
                this.stopTransmit(e);
            }
        });

        // Target selection
        this.targetSelect.addEventListener('change', () => this.sendTargetChange());

        // Priority selection
        if (this.prioritySelect) {
            this.prioritySelect.addEventListener('change', () => this.onPriorityChange());
        }

        // DND toggle
        if (this.dndToggle) {
            this.dndToggle.addEventListener('change', () => this.onDndChange());
        }

        // Keyboard support (spacebar)
        document.addEventListener('keydown', (e) => {
            if (e.code === 'Space' && !e.repeat && this.isConnected) {
                e.preventDefault();
                this.startTransmit(e);
            }
        });
        document.addEventListener('keyup', (e) => {
            if (e.code === 'Space') {
                e.preventDefault();
                this.stopTransmit(e);
            }
        });

        // Suspend audio/websocket/mic when app is backgrounded
        document.addEventListener('visibilitychange', () => {
            if (document.hidden) {
                this.suspend();
            } else {
                this.resume();
            }
        });
        window.addEventListener('pagehide', () => this.suspend());
        window.addEventListener('pageshow', () => this.resume());
    }

    showError(message) {
        this.errorBanner.textContent = message;
        this.errorBanner.classList.add('visible');
    }

    hideError() {
        this.errorBanner.classList.remove('visible');
    }

    updateStatus(status) {
        this.pttStatus.textContent = status.charAt(0).toUpperCase() + status.slice(1);
        this.pttStatus.className = 'status-value status-' + status;

        // Update button state — also apply priority class when transmitting
        this.pttButton.classList.remove('transmitting', 'receiving', 'busy',
                                        'priority-normal', 'priority-high', 'priority-emergency');
        if (status === 'transmitting') {
            this.pttButton.classList.add('transmitting');
            const priorityClasses = ['priority-normal', 'priority-high', 'priority-emergency'];
            const cls = priorityClasses[this.priority];
            if (cls) this.pttButton.classList.add(cls);
        } else if (status === 'receiving') {
            this.pttButton.classList.add('receiving');
        }
    }

    async initialize() {
        try {
            // Check for custom device name (only if not from URL param)
            if (!new URLSearchParams(window.location.search).get('device')) {
                const customName = this.deviceNameInput.value.trim();
                if (customName) {
                    localStorage.setItem('intercom_custom_name', customName);
                    this.clientId = customName;
                    console.log('Using custom device name:', this.clientId);
                } else {
                    localStorage.removeItem('intercom_custom_name');
                    this.clientId = this.getOrCreateClientId();
                }
            }

            // Check if we're in a secure context (HTTPS or localhost)
            if (!window.isSecureContext) {
                console.warn('Microphone requires HTTPS. Receive-only mode.');
                this.showError('Microphone requires HTTPS. You can listen but not talk.');
                this.micEnabled = false;

                // Create audio context for playback only
                this.audioContext = new AudioContext({ sampleRate: 16000 });
            } else {
                // Acquire microphone and set up audio
                await this.setupMic();
            }

            // Hide init overlay
            this.initOverlay.classList.add('hidden');
            this.isInitialized = true;

            // Connect WebSocket
            this.connectWebSocket();

        } catch (error) {
            console.error('Initialization error:', error);
            if (error.name === 'NotAllowedError') {
                this.showError('Microphone permission denied. Please allow access and reload.');
            } else if (error.name === 'NotFoundError') {
                this.showError('No microphone found. Please connect a microphone.');
            } else {
                this.showError('Failed to initialize: ' + error.message);
            }

            // Still try to connect for receive-only
            this.initOverlay.classList.add('hidden');
            this.isInitialized = true;
            this.micEnabled = false;
            this.audioContext = new AudioContext({ sampleRate: 16000 });
            this.connectWebSocket();
        }
    }

    async setupMic() {
        // Request microphone
        this.mediaStream = await navigator.mediaDevices.getUserMedia({
            audio: {
                echoCancellation: true,
                noiseSuppression: true,
                autoGainControl: true,
                sampleRate: 16000,
                channelCount: 1
            }
        });

        // Create audio context (16kHz to match protocol)
        this.audioContext = new AudioContext({ sampleRate: 16000 });

        // Load AudioWorklet
        await this.audioContext.audioWorklet.addModule('ptt-processor.js');

        // Create worklet node
        this.workletNode = new AudioWorkletNode(this.audioContext, 'ptt-processor');

        // Handle audio frames from worklet
        this.workletNode.port.onmessage = (event) => {
            if (event.data.type === 'audio' && this.isConnected && this.isTransmitting) {
                this.sendAudio(event.data.pcm);
            }
        };

        // Connect microphone to worklet
        this.mediaSource = this.audioContext.createMediaStreamSource(this.mediaStream);
        this.mediaSource.connect(this.workletNode);

        this.micEnabled = true;
    }

    releaseMic() {
        // Disconnect worklet
        if (this.mediaSource) {
            this.mediaSource.disconnect();
            this.mediaSource = null;
        }
        if (this.workletNode) {
            this.workletNode.disconnect();
            this.workletNode = null;
        }

        // Stop all mic tracks - this releases the mic
        if (this.mediaStream) {
            this.mediaStream.getTracks().forEach(track => track.stop());
            this.mediaStream = null;
        }

        this.micEnabled = false;
    }

    connectWebSocket() {
        // Clear any pending reconnect timer to prevent stacked connections
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }

        // Determine WebSocket URL - use relative path for ingress compatibility
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        const basePath = window.location.pathname.replace(/\/$/, '');
        const wsUrl = `${protocol}//${window.location.host}${basePath}/ws`;

        console.log('Connecting to WebSocket:', wsUrl);

        this.websocket = new WebSocket(wsUrl);
        this.websocket.binaryType = 'arraybuffer';

        this.websocket.onopen = () => {
            console.log('WebSocket connected');
            this.isConnected = true;
            this.reconnectDelay = 1000;  // Reset backoff on successful connection
            this.connStatus.textContent = 'Connected';
            this.connStatus.className = 'status-value status-connected';
            this.pttButton.classList.remove('disabled');
            this.callButton.disabled = false;
            this.hideError();

            // Send client identity if known (from notification URL)
            if (this.clientId) {
                this.websocket.send(JSON.stringify({
                    type: 'identify',
                    client_id: this.clientId
                }));
                console.log('Sent identity:', this.clientId);
            }

            // Request current state and target list
            this.websocket.send(JSON.stringify({ type: 'get_state' }));
        };

        this.websocket.onclose = () => {
            console.log('WebSocket disconnected');
            this.isConnected = false;
            this.connStatus.textContent = 'Disconnected';
            this.connStatus.className = 'status-value status-disconnected';
            this.pttButton.classList.add('disabled');
            this.callButton.disabled = true;
            this.updateStatus('idle');

            // Reconnect with exponential backoff (only if initialized and not hidden)
            if (this.isInitialized && !document.hidden) {
                const delay = this.reconnectDelay;
                console.log(`Reconnecting in ${delay / 1000}s...`);
                this.connStatus.textContent = `Reconnecting in ${delay / 1000}s...`;
                this.reconnectDelay = Math.min(this.reconnectDelay * 2, this.maxReconnectDelay);
                this.reconnectTimer = setTimeout(() => this.connectWebSocket(), delay);
            }
        };

        this.websocket.onerror = (error) => {
            console.error('WebSocket error:', error);
            this.showError('Connection error. Retrying...');
        };

        this.websocket.onmessage = (event) => {
            if (event.data instanceof ArrayBuffer) {
                // Binary data = audio PCM
                this.playAudio(event.data);
            } else {
                // JSON message
                try {
                    const msg = JSON.parse(event.data);
                    this.handleMessage(msg);
                } catch (e) {
                    console.error('Failed to parse message:', e);
                }
            }
        };
    }

    handleMessage(msg) {
        switch (msg.type) {
            case 'init':
                // Initial connection - set version and status
                if (msg.version) {
                    const versionEl = document.querySelector('.version');
                    if (versionEl) versionEl.textContent = `v${msg.version}`;
                }
                if (msg.status) {
                    this.updateStatus(msg.status);
                }
                // If server suggests a name and we don't have one from URL or custom storage
                if (msg.suggested_name && !new URLSearchParams(window.location.search).get('device')) {
                    const customName = localStorage.getItem('intercom_custom_name');
                    if (!customName) {
                        // Use suggested name (mobile app detected)
                        this.clientId = msg.suggested_name;
                        console.log('Using suggested name from server:', this.clientId);
                        // Re-send identity with the suggested name
                        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
                            this.websocket.send(JSON.stringify({
                                type: 'identify',
                                client_id: this.clientId
                            }));
                        }
                    }
                }
                break;

            case 'state':
                this.updateStatus(msg.status);
                if (msg.status === 'receiving') {
                    this.isReceiving = true;
                } else {
                    this.isReceiving = false;
                }
                break;

            case 'targets':
                // Update target dropdown
                this.targetSelect.innerHTML = '<option value="all">All Rooms</option>';
                if (msg.rooms) {
                    msg.rooms.forEach(room => {
                        const option = document.createElement('option');
                        option.value = room;
                        option.textContent = room;
                        this.targetSelect.appendChild(option);
                    });
                }
                // Auto-select caller if coming from notification
                if (this.pendingCaller) {
                    const callerOption = Array.from(this.targetSelect.options).find(
                        opt => opt.value.toLowerCase().includes(this.pendingCaller.toLowerCase())
                    );
                    if (callerOption) {
                        this.targetSelect.value = callerOption.value;
                        console.log('Auto-selected caller:', callerOption.value);
                    }
                    this.pendingCaller = null; // Clear so it doesn't re-select
                }
                break;

            case 'recent_call':
                // Auto-select caller from recent notification
                if (msg.caller) {
                    const callerOption = Array.from(this.targetSelect.options).find(
                        opt => opt.value.toLowerCase().includes(msg.caller.toLowerCase())
                    );
                    if (callerOption) {
                        this.targetSelect.value = callerOption.value;
                        console.log('Auto-selected recent caller:', callerOption.value);
                    }
                }
                break;

            case 'busy':
                // Channel is busy
                this.pttButton.classList.add('busy');
                this.updateStatus('busy');
                break;

            case 'error':
                this.showError(msg.message);
                break;
        }
    }

    startTransmit(event) {
        console.log('startTransmit called, isConnected:', this.isConnected, 'isTransmitting:', this.isTransmitting);
        if (!this.isConnected || this.isTransmitting) return;

        // Check if mic is available
        if (!this.micEnabled) {
            this.showError('Microphone not available.');
            return;
        }

        // Check if receiving (channel busy)
        if (this.isReceiving) {
            this.pttButton.classList.add('busy');
            return;
        }

        // Suppress PTT for 2s after sending a call to prevent the chime acoustic
        // tail from being captured by the mic and broadcast to the called room.
        if (this.lastCallTime > 0 && (Date.now() - this.lastCallTime) < 2000) {
            console.log('PTT suppressed: waiting for call tone to decay');
            return;
        }

        this.isTransmitting = true;
        this.pttButton.classList.add('active', 'transmitting');
        this.updateStatus('transmitting');
        console.log('PTT started');

        // Tell worklet to start capturing
        if (this.workletNode) {
            this.workletNode.port.postMessage({ type: 'ptt', active: true });
        }

        // Tell server we're transmitting — include selected priority
        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
            const priorityNames = ['Normal', 'High', 'Emergency'];
            this.websocket.send(JSON.stringify({
                type: 'ptt_start',
                target: this.targetSelect.value,
                priority: priorityNames[this.priority] || 'Normal'
            }));
        }
    }

    stopTransmit(event) {
        console.log('stopTransmit called, isTransmitting:', this.isTransmitting);

        // Always clean up UI state
        this.pttButton.classList.remove('active', 'transmitting', 'busy');

        if (!this.isTransmitting) {
            return;
        }

        this.isTransmitting = false;
        this.updateStatus('idle');
        console.log('PTT stopped');

        // Tell worklet to stop capturing
        if (this.workletNode) {
            this.workletNode.port.postMessage({ type: 'ptt', active: false });
        }

        // Tell server we stopped
        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
            this.websocket.send(JSON.stringify({ type: 'ptt_stop' }));
        }
    }

    sendAudio(pcmBuffer) {
        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
            this.websocket.send(pcmBuffer);
        }
    }

    sendTargetChange() {
        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
            this.websocket.send(JSON.stringify({
                type: 'set_target',
                target: this.targetSelect.value
            }));
        }
    }

    sendCall() {
        const target = this.targetSelect.value;
        if (!this.isConnected) {
            return;
        }

        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
            this.websocket.send(JSON.stringify({
                type: 'call',
                target: target
            }));
            console.log('Sent call to:', target);
            this.lastCallTime = Date.now();  // Start PTT lockout

            // Brief visual feedback
            this.callButton.classList.add('calling');
            setTimeout(() => {
                this.callButton.classList.remove('calling');
            }, 1000);
        }
    }

    playAudio(rawBuffer) {
        if (!this.audioContext) return;

        // Parse the 1-byte priority prefix prepended by the hub
        // Frame format: [1 byte priority][PCM Int16LE data...]
        const rawBytes = new Uint8Array(rawBuffer);
        if (rawBytes.length < 2) return;

        const incomingPriority = rawBytes[0];
        const pcmBuffer = rawBuffer.slice(1);  // PCM data without the prefix byte

        // Client-side DND: only play EMERGENCY audio
        if (this.dndEnabled && incomingPriority < PRIORITY_EMERGENCY) {
            return;
        }

        // Resume context if needed (mobile browsers suspend it)
        if (this.audioContext.state === 'suspended') {
            this.audioContext.resume();
        }

        // Convert Int16 to Float32, reusing buffer when possible
        const int16 = new Int16Array(pcmBuffer);
        const numSamples = int16.length;

        // Reuse pre-allocated buffer if it matches the expected size, otherwise allocate
        if (!this.playbackBuffer || this.playbackBuffer.length !== numSamples) {
            this.playbackBuffer = new Float32Array(numSamples);
        }
        const float32 = this.playbackBuffer;

        for (let i = 0; i < numSamples; i++) {
            float32[i] = int16[i] / 32768.0;
        }

        // Create audio buffer
        const buffer = this.audioContext.createBuffer(1, float32.length, 16000);
        buffer.getChannelData(0).set(float32);

        // Create source and schedule playback
        const source = this.audioContext.createBufferSource();
        source.buffer = buffer;
        source.connect(this.audioContext.destination);

        // Schedule to play in sequence
        const now = this.audioContext.currentTime;
        if (this.nextPlayTime < now) {
            this.nextPlayTime = now + 0.02; // Small buffer
        }
        source.start(this.nextPlayTime);
        this.nextPlayTime += buffer.duration;
    }

    onPriorityChange() {
        const val = parseInt(this.prioritySelect.value, 10);
        if (val >= PRIORITY_NORMAL && val <= PRIORITY_EMERGENCY) {
            this.priority = val;
            console.log('Priority set to:', ['Normal', 'High', 'Emergency'][val]);
            // Update UI styling to reflect the selected priority
            this.prioritySelect.className = 'priority-select priority-' + ['normal', 'high', 'emergency'][val];
        }
    }

    onDndChange() {
        this.dndEnabled = this.dndToggle.checked;
        console.log('DND', this.dndEnabled ? 'enabled' : 'disabled');
    }

    suspend() {
        if (!this.isInitialized) return;

        console.log('Suspending - app backgrounded');

        // Stop any active transmission
        if (this.isTransmitting) {
            this.stopTransmit();
        }

        // Release microphone
        this.releaseMic();

        // Cancel any pending reconnect timer
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }

        // Close WebSocket to stop receiving audio
        if (this.websocket) {
            this.websocket.onclose = null; // Prevent auto-reconnect
            this.websocket.close();
            this.websocket = null;
        }
        this.isConnected = false;

        // Close audio context completely
        if (this.audioContext) {
            this.nextPlayTime = 0;
            this.audioContext.close();
            this.audioContext = null;
        }

        // Update UI
        this.connStatus.textContent = 'Suspended';
        this.connStatus.className = 'status-value status-disconnected';
        this.pttButton.classList.add('disabled');
    }

    async resume() {
        if (!this.isInitialized) return;

        console.log('Resuming - app foregrounded');
        this.nextPlayTime = 0;

        try {
            // Re-setup microphone and audio context
            if (window.isSecureContext) {
                await this.setupMic();
            } else {
                this.audioContext = new AudioContext({ sampleRate: 16000 });
            }

            // Reset backoff so reconnect is immediate after resume
            this.reconnectDelay = 1000;

            // Reconnect WebSocket
            this.connectWebSocket();

        } catch (error) {
            console.error('Resume error:', error);
            this.showError('Failed to resume: ' + error.message);

            // Try to at least connect for receive-only
            this.audioContext = new AudioContext({ sampleRate: 16000 });
            this.connectWebSocket();
        }
    }

    getOrCreateClientId() {
        const storageKey = 'intercom_client_id';

        // Check localStorage for existing ID
        let clientId = localStorage.getItem(storageKey);
        if (clientId) {
            return clientId;
        }

        // Generate a friendly name: "Web_XXXX" where XXXX is random hex
        const randomPart = Math.random().toString(16).substring(2, 6).toUpperCase();
        clientId = `Web_${randomPart}`;

        // Store for future sessions
        localStorage.setItem(storageKey, clientId);
        return clientId;
    }
}

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    window.intercomPTT = new IntercomPTT();
});
