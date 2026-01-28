/**
 * Intercom PTT Web Client
 *
 * Connects to the Intercom Hub add-on via WebSocket and provides
 * push-to-talk functionality from any web browser.
 *
 * Mic is acquired on init, released when app is backgrounded,
 * and re-acquired when app returns to foreground.
 */

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
        this.isInitialized = false;
        this.micEnabled = false;

        // Audio playback
        this.nextPlayTime = 0;

        // UI Elements
        this.initOverlay = document.getElementById('initOverlay');
        this.initButton = document.getElementById('initButton');
        this.pttButton = document.getElementById('pttButton');
        this.connStatus = document.getElementById('connStatus');
        this.pttStatus = document.getElementById('pttStatus');
        this.targetSelect = document.getElementById('targetSelect');
        this.errorBanner = document.getElementById('errorBanner');

        // Bind event handlers
        this.initButton.addEventListener('click', () => this.initialize());

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

        // Update button state
        this.pttButton.classList.remove('transmitting', 'receiving', 'busy');
        if (status === 'transmitting') {
            this.pttButton.classList.add('transmitting');
        } else if (status === 'receiving') {
            this.pttButton.classList.add('receiving');
        }
    }

    async initialize() {
        try {
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
            this.connStatus.textContent = 'Connected';
            this.connStatus.className = 'status-value status-connected';
            this.pttButton.classList.remove('disabled');
            this.hideError();

            // Request current state and target list
            this.websocket.send(JSON.stringify({ type: 'get_state' }));
        };

        this.websocket.onclose = () => {
            console.log('WebSocket disconnected');
            this.isConnected = false;
            this.connStatus.textContent = 'Disconnected';
            this.connStatus.className = 'status-value status-disconnected';
            this.pttButton.classList.add('disabled');
            this.updateStatus('idle');

            // Reconnect after delay (only if initialized and not hidden)
            if (this.isInitialized && !document.hidden) {
                setTimeout(() => this.connectWebSocket(), 3000);
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

        this.isTransmitting = true;
        this.pttButton.classList.add('active', 'transmitting');
        this.updateStatus('transmitting');
        console.log('PTT started');

        // Tell worklet to start capturing
        if (this.workletNode) {
            this.workletNode.port.postMessage({ type: 'ptt', active: true });
        }

        // Tell server we're transmitting
        if (this.websocket && this.websocket.readyState === WebSocket.OPEN) {
            this.websocket.send(JSON.stringify({
                type: 'ptt_start',
                target: this.targetSelect.value
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

    playAudio(pcmBuffer) {
        if (!this.audioContext) return;

        // Resume context if needed (mobile browsers suspend it)
        if (this.audioContext.state === 'suspended') {
            this.audioContext.resume();
        }

        // Convert Int16 to Float32
        const int16 = new Int16Array(pcmBuffer);
        const float32 = new Float32Array(int16.length);

        for (let i = 0; i < int16.length; i++) {
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

    suspend() {
        if (!this.isInitialized) return;

        console.log('Suspending - app backgrounded');

        // Stop any active transmission
        if (this.isTransmitting) {
            this.stopTransmit();
        }

        // Release microphone
        this.releaseMic();

        // Close WebSocket to stop receiving audio
        if (this.websocket) {
            this.websocket.onclose = null; // Prevent auto-reconnect
            this.websocket.close();
            this.websocket = null;
        }
        this.isConnected = false;

        // Close audio context completely
        if (this.audioContext) {
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

        try {
            // Re-setup microphone and audio context
            if (window.isSecureContext) {
                await this.setupMic();
            } else {
                this.audioContext = new AudioContext({ sampleRate: 16000 });
            }

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
}

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
    window.intercomPTT = new IntercomPTT();
});
