var gateway = `ws://${window.location.hostname}/ws`;
var websocket;

// -------------------------------- Init --------------------------------- //

window.addEventListener('load', onLoad);

function onLoad(event) {
    initWebSocket();
    initUI();
    createHearts(); 
}

function initUI() {
    // Set the initial slider label
    var slider = document.getElementById('brightnessSlider');
    if (slider) {
        updateSliderLabel(slider.value);
    }
}

function updateSliderLabel(value) {
     document.getElementById('brightnessLabel').innerText = value;
}

// ---------------------- Heart Background ---------------------- //

function createHearts() {
    const container = document.querySelector('.heart-container');
    if (!container) return; 

    for (let i = 0; i < 20; i++) { // Create 20 hearts
        const heart = document.createElement('div');
        heart.classList.add('heart');
        heart.innerHTML = '♥';
        
        // Randomize the start
        heart.style.left = Math.random() * 100 + 'vw'; // Random horizontal position
        heart.style.animationDelay = Math.random() * 15 + 's'; // Random start time
        heart.style.animationDuration = (Math.random() * 10 + 10) + 's'; // Random speed
        heart.style.fontSize = (Math.random() * 20 + 10) + 'px'; // Random size
        heart.style.opacity = Math.random() * 0.5 + 0.2; // Random fade

        container.appendChild(heart);
    }
}


// ---------------------------- WebSocket ---------------------------- //

function initWebSocket() {
    console.log('Trying to open a WebSocket connection...');
    websocket = new WebSocket(gateway);
    websocket.onopen    = onOpen;
    websocket.onclose   = onClose;
    websocket.onmessage = onMessage; 
}

function onOpen(event) {
    console.log('Connection opened');
    document.getElementById('wsState').innerText = "Connected";
    requestState(); // Request current state once connected
}

function onClose(event) {
    console.log('Connection closed');
    document.getElementById('wsState').innerText = "Closed";
    setTimeout(initWebSocket, 2000); // Try to reconnect
}

// Function to handle messages received from the server
function onMessage(event) {
    console.log("Received: ", event.data);
    try {
        var state = JSON.parse(event.data);

        if (state.mode !== undefined) { 
            
            // --- 1. ADDED: Live UI Heart Logic ---
            const liveHeart = document.getElementById('liveHeartIcon');

            if (state.color && state.mode === 'static') {
                liveHeart.style.fill = state.color; // Set to static color
            } else if (state.mode === 'breath') {
                liveHeart.style.fill = '#3278dc'; // The breathing blue
            } else if (state.mode === 'favorite') {
                liveHeart.style.fill = '#e6a8ff'; // A shimmery purple for "sparkle"
            } else if (state.mode === 'weather') {
                liveHeart.style.fill = '#87ceeb'; // A "sky" blue
            } else if (state.mode === 'night') {
                liveHeart.style.fill = '#ffaa00'; // The warm nightlight color
            } else {
                liveHeart.style.fill = '#cccccc'; // Default grey
            }

            // --- 2. Update Status Text Fields ---
            if (state.mode !== undefined) {
                document.getElementById('currentMode').innerText = state.mode;
            }
            if (state.brightness !== undefined) {
                document.getElementById('currentBrightness').innerText = state.brightness;
            }
            if (state.color !== undefined && state.mode === 'static') {
                document.getElementById('currentColor').innerText = state.color;
            } else {
                document.getElementById('currentColor').innerText = "---"; 
            }
            if (state.weather !== undefined) {
                document.getElementById('currentWeather').innerText = state.weather;
            } else {
                document.getElementById('currentWeather').innerText = "---";
            }
        
        } else if (state.status) {
            console.log("Status message from ESP32:", state.status);
        } else if (state.error) {
            console.error("Error from ESP32:", state.error);
        }

    } catch (e) {
        console.error("Could not parse JSON: ", e);
    }
}

// ------------------------- Send Functions ------------------------- //

function sendData(jsonData) {
    if (websocket.readyState === WebSocket.OPEN) {
        console.log("Sending:", JSON.stringify(jsonData));
        websocket.send(JSON.stringify(jsonData));
    } else {
        console.log("WebSocket is not open.");
    }
}

function sendColor() {
    var color = document.getElementById('colorPicker').value;
    // Use the correct action name from your C++
    sendData({ action: "setcolor", value: color });
}

function sendOff() {
     sendData({ action: "setcolor", value: "off" });
}

function sendBrightness() {
    var brightness = document.getElementById('brightnessSlider').value;
    // Use the correct action name from your C++
    sendData({ action: "setbrightness", value: parseInt(brightness) });
}

function sendMode(modeName) {
    sendData({ action: "setmode", value: modeName });
}


function requestState() {
     sendData({ action: "getstate" });
}